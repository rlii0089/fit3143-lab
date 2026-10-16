//////////////////////////////////////////////////////////////////////////////////////
// wsn_v3.c - FIT3143 Lab 3, Version 3: cached subscriptions + change-only updates
// ----------------------------------------------------------------------------------
// Uses the SAME common.h / common.c interfaces and command-line configuration as V2.
// Each EV node remains one Cartesian MPI process; the base station is one process.
// OpenMP charging-port updates continue to use update_charging_ports() from common.c.
//
// Changes relative to V2:
//  1. A congested node SUBSCRIBES to each neighbour once (MPI_Isend).
//  2. Each subscribed neighbour sends an initial port-status message and then
//     sends status changes ONLY when its free-port count changes.
//  3. A node uses the most recent cached neighbour states. It does not poll
//     the same neighbours again on every charging-port update.
//  4. An unchanged, continuously congested incident sends ONE alert, with
//     periodic refreshes. A changed congestion state sends a new alert.
//  5. The base station still uses MPI_Irecv/MPI_Test and sends nonblocking advice.
//
// The cache is eventually consistent: a very recent remote change may still be
// in flight when a decision is taken. Validate this trade-off in the report.
// Refresh period MUST be shorter than the common.c base-station report window.
// Compile with the original project common.c + this file, not V2 in the same link.
// Example: mpirun --oversubscribe -np 17 ./wsn_v3 4 4 --scenario corner
//          --rounds 3 --interval-ms 500
//////////////////////////////////////////////////////////////////////////////////////

#include "common.h"

#define VERSION "V3"
#define TAG_V3_SUBSCRIBE 410
#define TAG_V3_STATUS    411

// Set to a duration shorter than your --window-ms base-station expiry period.
// For the default 30000 ms report window, 10000 ms is appropriate.
#ifndef V3_REPORT_REFRESH_MS
#define V3_REPORT_REFRESH_MS 10000
#endif

typedef struct
{
    ChargingNode node;
    CommStats stats;
    int cycle;
    int terminating;
    int in_barrier;
    int requests_serviced;

    // Persistent subscription server: any adjacent node can subscribe.
    NbrRequest subscribe_in;
    MPI_Request subscribe_recv;
    int subscribers[NUM_NEIGHBOURS];
    int status_dirty[NUM_NEIGHBOURS];
    NbrResponse status_out[NUM_NEIGHBOURS];
    MPI_Request status_send[NUM_NEIGHBOURS];

    // Persistent subscription client: subscribe once and cache subsequent updates.
    int subscribed_to[NUM_NEIGHBOURS];
    int cache_valid[NUM_NEIGHBOURS];
    int cache_free[NUM_NEIGHBOURS];
    int initial_status_pending;
    double subscription_started_at;
    double initial_exchange_seconds;
    int decision_dirty;                 // Avoid evaluating on every poll() call
    NbrRequest subscribe_out;
    MPI_Request subscribe_send[NUM_NEIGHBOURS];
    NbrResponse status_in;
    MPI_Request status_recv;

    // One congestion incident can span many update cycles.
    int incident_open;
    int last_incident_free;
    int last_incident_neighbours[NUM_NEIGHBOURS];
    double last_report_at;
    int last_redirect_cycle;
    long long duplicate_alerts_saved;
    long long cache_reuse_cycles;

    // Actual neighbour messages (not fictitious collective slots) in this report.
    int nbr_messages_sent_since_report;
    int nbr_messages_received_since_report;
    int nbr_bytes_sent_since_report;
    int nbr_bytes_received_since_report;

    // Congestion report and base-station reply (same types as V2).
    int awaiting_advice;
    NodeReport report;
    MPI_Request report_send;
    Advice advice_in;
    MPI_Request advice_recv;

    // Termination and nonblocking shutdown barrier.
    Termination term_in;
    MPI_Request term_recv;
} NodeState;

/*
 * Function: direction_of
 * Purpose : Finds TOP/BOTTOM/LEFT/RIGHT for the MPI rank of a Cartesian
 *           neighbour. Rejects unexpected senders, as in V2.
 */
int direction_of(const ChargingNode *node, int rank)
{
    int i;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
        if (node->neighbours[i] == rank) return i;
    fprintf(stderr, "[Node %d] Unexpected non-neighbour rank %d.\n", node->rank, rank);
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    return -1;
}

/*
 * Function: count_neighbour_send
 * Purpose : Counts real point-to-point application messages and bytes sent to
 *           neighbours, separately from report/advice/termination messages.
 */
void count_neighbour_send(NodeState *st, int bytes)
{
    stats_count_message(&st->stats, bytes);
    st->nbr_messages_sent_since_report++;
    st->nbr_bytes_sent_since_report += bytes;
}

/*
 * Function: prepare_status_updates
 * Purpose : When the local free-port count changes, marks all previously
 *           subscribed neighbours for one fresh notification. Multiple
 *           changes may be coalesced while a previous send is in flight.
 */
void prepare_status_updates(NodeState *st)
{
    int i;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
        if (st->subscribers[i]) st->status_dirty[i] = 1;
}

/*
 * Function: service_subscriptions
 * Purpose : Receives and records new neighbour subscriptions, then re-posts
 *           the receive to handle multiple neighbours without deadlock.
 *           The first status reply is scheduled by setting status_dirty.
 */
void service_subscriptions(NodeState *st, MPI_Comm cart_comm)
{
    int ready, dir;
    MPI_Status status;
    MPI_Test(&st->subscribe_recv, &ready, &status);
    while (ready)
    {
        dir = direction_of(&st->node, status.MPI_SOURCE);
        stats_record(&st->stats, DELAY_NBR_REQ,
                     global_time() - st->subscribe_in.sent_at);
        st->subscribers[dir] = 1;
        st->status_dirty[dir] = 1;
        st->requests_serviced++;
        st->nbr_messages_received_since_report++;
        st->nbr_bytes_received_since_report += (int)sizeof(NbrRequest);

        MPI_Irecv(&st->subscribe_in, (int)sizeof(NbrRequest), MPI_BYTE,
                  MPI_ANY_SOURCE, TAG_V3_SUBSCRIBE, cart_comm, &st->subscribe_recv);
        MPI_Test(&st->subscribe_recv, &ready, &status);
    }
}

/*
 * Function: flush_status_updates
 * Purpose : Sends one most-recent availability update per subscribed neighbour
 *           whenever a change is pending. Reuses a send buffer ONLY after the
 *           previous MPI_Isend request completes. No periodic status spam.
 */
void flush_status_updates(NodeState *st, MPI_Comm cart_comm)
{
    int i, completed;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        if (!st->subscribers[i] || !st->status_dirty[i]) continue;
        MPI_Test(&st->status_send[i], &completed, MPI_STATUS_IGNORE);
        if (!completed) continue;
        st->status_out[i].free_ports = st->node.free_ports;
        st->status_out[i].sent_at = global_time();
        MPI_Isend(&st->status_out[i], (int)sizeof(NbrResponse), MPI_BYTE,
                  st->node.neighbours[i], TAG_V3_STATUS, cart_comm,
                  &st->status_send[i]);
        count_neighbour_send(st, (int)sizeof(NbrResponse));
        st->status_dirty[i] = 0;
    }
}

/*
 * Function: start_subscriptions
 * Purpose : Subscribes to the four valid neighbouring nodes only the FIRST
 *           time the node needs congestion information. A receive for future
 *           status updates is already permanently posted.
 */
void start_subscriptions(NodeState *st, MPI_Comm cart_comm)
{
    int i;
    // Reuse existing subscriptions on later cycles.
    // Do not overwrite the nonblocking send buffer while it is in flight.
    if (st->subscribed_to[0] || st->subscribed_to[1] ||
        st->subscribed_to[2] || st->subscribed_to[3]) return;
    st->subscription_started_at = global_time();
    st->subscribe_out.sent_at = st->subscription_started_at;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        int neighbour = st->node.neighbours[i];
        if (neighbour == MPI_PROC_NULL || st->subscribed_to[i]) continue;
        st->subscribed_to[i] = 1;
        st->initial_status_pending++;
        MPI_Isend(&st->subscribe_out, (int)sizeof(NbrRequest), MPI_BYTE,
                  neighbour, TAG_V3_SUBSCRIBE, cart_comm, &st->subscribe_send[i]);
        count_neighbour_send(st, (int)sizeof(NbrRequest));
    }
}

/*
 * Function: receive_status_updates
 * Purpose : Collects initial availability replies and later change notices.
 *           The newest free-port count overwrites the local cache. Records
 *           initial-subscription round trip once ALL neighbours have replied.
 */
void receive_status_updates(NodeState *st, MPI_Comm cart_comm)
{
    int ready, dir;
    MPI_Status status;
    MPI_Test(&st->status_recv, &ready, &status);
    while (ready)
    {
        dir = direction_of(&st->node, status.MPI_SOURCE);
        stats_record(&st->stats, DELAY_NBR_RESP,
                     global_time() - st->status_in.sent_at);
        st->nbr_messages_received_since_report++;
        st->nbr_bytes_received_since_report += (int)sizeof(NbrResponse);

        if (!st->cache_valid[dir])
        {
            st->cache_valid[dir] = 1;
            st->initial_status_pending--;
            if (st->initial_status_pending == 0)
            {
                st->initial_exchange_seconds = global_time() - st->subscription_started_at;
                stats_record(&st->stats, DELAY_NBR_RTT,
                             st->initial_exchange_seconds);
            }
        }
        if (st->cache_free[dir] != st->status_in.free_ports)
            st->decision_dirty = 1;
        st->cache_free[dir] = st->status_in.free_ports;
        MPI_Irecv(&st->status_in, (int)sizeof(NbrResponse), MPI_BYTE,
                  MPI_ANY_SOURCE, TAG_V3_STATUS, cart_comm, &st->status_recv);
        MPI_Test(&st->status_recv, &ready, &status);
    }
}

/*
 * Function: all_cache_ready
 * Purpose : Checks that every real neighbouring node has supplied an initial
 *           value before a congestion/redirect decision uses cached data.
 */
int all_cache_ready(const NodeState *st)
{
    int i;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
        if (st->node.neighbours[i] != MPI_PROC_NULL && !st->cache_valid[i])
            return 0;
    return 1;
}

/*
 * Function: incident_changed
 * Purpose : Detects a new congestion incident or a change in this node's or a
 *           neighbour's availability since the previous incident alert.
 */
int incident_changed(const NodeState *st)
{
    int i;
    if (!st->incident_open || st->last_incident_free != st->node.free_ports)
        return 1;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
        if (st->last_incident_neighbours[i] != st->cache_free[i]) return 1;
    return 0;
}

/*
 * Function: send_report
 * Purpose : Sends an alert only when a congestion incident is new, its state
 *           changes, or its periodic refresh is due. The report records actual
 *           subscription and status-update traffic since the previous alert.
 */
void send_report(NodeState *st, const Config *cfg, int base_rank, double now)
{
    NodeReport *r = &st->report;
    int i;
    memset(r, 0, sizeof(*r));
    r->rank = st->node.rank;
    r->row = st->node.coords[0];
    r->col = st->node.coords[1];
    r->free_ports = st->node.free_ports;
    r->occupied_ports = cfg->ports - st->node.free_ports;
    r->port_threads = st->node.port_threads;
    r->congested = 1;
    r->alert = 1;
    r->redirect_rank = -1;
    r->incoming_requests = st->requests_serviced;
    r->neighbour_exchange_seconds = st->initial_exchange_seconds;
    r->neighbour_messages_sent = st->nbr_messages_sent_since_report;
    r->neighbour_messages_received = st->nbr_messages_received_since_report;
    r->neighbour_bytes_sent = st->nbr_bytes_sent_since_report;
    r->neighbour_bytes_received = st->nbr_bytes_received_since_report;
    current_timestamp(r->timestamp, TIME_LEN);
    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        r->neighbours[i] = st->node.neighbours[i];
        r->neighbour_free[i] = st->cache_free[i];
        r->requested[i] = st->subscribed_to[i];
        st->last_incident_neighbours[i] = st->cache_free[i];
    }
    st->last_incident_free = st->node.free_ports;
    st->incident_open = 1;
    st->last_report_at = now;
    st->nbr_messages_sent_since_report = 0;
    st->nbr_messages_received_since_report = 0;
    st->nbr_bytes_sent_since_report = 0;
    st->nbr_bytes_received_since_report = 0;

    // Advice receive must exist BEFORE the report can be answered.
    MPI_Irecv(&st->advice_in, (int)sizeof(Advice), MPI_BYTE, base_rank,
              TAG_ADVICE, MPI_COMM_WORLD, &st->advice_recv);
    r->sent_at = global_time();
    MPI_Isend(r, (int)sizeof(NodeReport), MPI_BYTE, base_rank,
              TAG_REPORT, MPI_COMM_WORLD, &st->report_send);
    stats_count_message(&st->stats, (int)sizeof(NodeReport));
    st->stats.alerts++;
    st->awaiting_advice = 1;
}

/*
 * Function: evaluate_congestion
 * Purpose : Uses cached neighbour availability to redirect locally, or reports
 *           new/changed congestion once. Reuses the previous decision when the
 *           state is unchanged, avoiding repeated subscriptions and alerts.
 */
void evaluate_congestion(NodeState *st, const Config *cfg, int base_rank)
{
    int redirect;
    double now = global_time();
    int refresh_due = st->incident_open &&
        now - st->last_report_at >= (double)V3_REPORT_REFRESH_MS / 1000.0;
    if (st->terminating || !all_cache_ready(st) || st->awaiting_advice) return;
    if (!st->decision_dirty && !refresh_due) return;
    st->decision_dirty = 0;
    if (st->node.free_ports >= cfg->threshold)
    {
        st->incident_open = 0;             // A recovered node can alert again later.
        return;
    }

    redirect = choose_redirect(cfg, st->node.neighbours, st->cache_free);
    if (redirect >= 0)
    {
        st->incident_open = 0;
        if (st->last_redirect_cycle != st->cycle)
        {
            st->last_redirect_cycle = st->cycle;
            st->stats.redirects++;
            printf("[Cycle %d][Node %d] %d free ports; redirect EVs to neighbour %d (cached).\n",
                   st->cycle, st->node.rank, st->node.free_ports, redirect);
            fflush(stdout);
        }
        return;
    }

    if (incident_changed(st) || refresh_due)
        send_report(st, cfg, base_rank, now);
    else
        st->duplicate_alerts_saved++;
}

/*
 * Function: check_advice
 * Purpose : Completes nonblocking base-station advice, times the complete
 *           report/advice round trip, and prints suggested charging nodes.
 */
void check_advice(NodeState *st)
{
    int ready, i;
    MPI_Test(&st->advice_recv, &ready, MPI_STATUS_IGNORE);
    if (!ready) return;
    stats_record(&st->stats, DELAY_ADVICE, global_time() - st->advice_in.sent_at);
    stats_record(&st->stats, DELAY_REPORT_RTT, global_time() - st->report.sent_at);
    MPI_Wait(&st->report_send, MPI_STATUS_IGNORE);
    st->awaiting_advice = 0;
    printf("[Cycle %d][Node %d] Congestion alert -> base. ", st->cycle, st->node.rank);
    if (st->advice_in.count == 0)
        printf("No available nearby nodes.\n");
    else
    {
        printf("Nearest available node(s) for EVs:");
        for (i = 0; i < st->advice_in.count; i++)
            printf(" %d", st->advice_in.nodes[i]);
        printf("\n");
    }
    fflush(stdout);
}

/*
 * Function: outbound_complete
 * Purpose : Tests that all this node's subscription requests and status
 *           updates have finished before it joins the shutdown barrier.
 *           MPI_Test also provides progress for nonblocking requests.
 */
int outbound_complete(NodeState *st)
{
    int i, ready, all_done = 1;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        MPI_Test(&st->subscribe_send[i], &ready, MPI_STATUS_IGNORE);
        if (!ready) all_done = 0;
        MPI_Test(&st->status_send[i], &ready, MPI_STATUS_IGNORE);
        if (!ready || st->status_dirty[i]) all_done = 0;
    }
    return all_done;
}

/*
 * Function: run_charging_node
 * Purpose : Runs independent port-update cycles, maintaining subscriptions and
 *           cached neighbour statuses. Continues servicing pending traffic
 *           during shutdown and frees MPI requests and node memory cleanly.
 */
void run_charging_node(const Config *cfg, MPI_Comm cart_comm, int base_rank)
{
    NodeState st;
    MPI_Request barrier = MPI_REQUEST_NULL;
    double next_cycle;
    int i, ready, done = 0;

    memset(&st, 0, sizeof(st));
    initialise_node(&st.node, cfg, cart_comm);
    print_node_topology(&st.node, cfg);
    st.last_redirect_cycle = -1;
    st.report_send = MPI_REQUEST_NULL;
    st.advice_recv = MPI_REQUEST_NULL;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        st.status_send[i] = MPI_REQUEST_NULL;
        st.subscribe_send[i] = MPI_REQUEST_NULL;
        st.cache_free[i] = -1;
        st.last_incident_neighbours[i] = -1;
    }

    MPI_Irecv(&st.subscribe_in, (int)sizeof(NbrRequest), MPI_BYTE,
              MPI_ANY_SOURCE, TAG_V3_SUBSCRIBE, cart_comm, &st.subscribe_recv);
    MPI_Irecv(&st.status_in, (int)sizeof(NbrResponse), MPI_BYTE,
              MPI_ANY_SOURCE, TAG_V3_STATUS, cart_comm, &st.status_recv);
    MPI_Irecv(&st.term_in, (int)sizeof(Termination), MPI_BYTE, base_rank,
              TAG_TERM, MPI_COMM_WORLD, &st.term_recv);

    next_cycle = global_time();
    while (!done)
    {
        double now = global_time();
        if (!st.terminating && now >= next_cycle)
        {
            int previous_free = st.node.free_ports;
            st.cycle++;
            st.stats.cycles++;
            st.requests_serviced = 0;
            st.decision_dirty = 1;
            if (all_cache_ready(&st) && st.subscribed_to[0] + st.subscribed_to[1] +
                st.subscribed_to[2] + st.subscribed_to[3] > 0)
                st.cache_reuse_cycles++;
            update_charging_ports(&st.node, cfg, st.cycle);
            if (previous_free != st.node.free_ports)
                prepare_status_updates(&st);
            next_cycle += cfg->interval_ms / 1000.0;
            if (next_cycle < now) next_cycle = now;

            if (st.node.free_ports < cfg->threshold)
                start_subscriptions(&st, cart_comm);
            else
                st.incident_open = 0;
        }

        service_subscriptions(&st, cart_comm);
        flush_status_updates(&st, cart_comm);
        receive_status_updates(&st, cart_comm);
        if (st.awaiting_advice) check_advice(&st);
        if (!st.terminating) evaluate_congestion(&st, cfg, base_rank);

        if (!st.terminating)
        {
            MPI_Test(&st.term_recv, &ready, MPI_STATUS_IGNORE);
            if (ready)
            {
                st.terminating = 1;
                stats_record(&st.stats, DELAY_TERM,
                             global_time() - st.term_in.sent_at);
            }
        }

        // Do not start new requests/reports after TERM. Finish earlier traffic,
        // but continue answering incoming subscriptions until all ranks agree.
        if (st.terminating && !st.in_barrier &&
            !st.awaiting_advice && st.initial_status_pending == 0 &&
            outbound_complete(&st))
        {
            MPI_Ibarrier(MPI_COMM_WORLD, &barrier);
            st.in_barrier = 1;
        }
        if (st.in_barrier) MPI_Test(&barrier, &done, MPI_STATUS_IGNORE);
        if (!done) sleep_microseconds(cfg->poll_us);
    }

    // Complete sends before cancelling unmatched standing receives. Requests
    // posted by other ranks before TERM were handled during the barrier.
    MPI_Waitall(NUM_NEIGHBOURS, st.subscribe_send, MPI_STATUSES_IGNORE);
    MPI_Waitall(NUM_NEIGHBOURS, st.status_send, MPI_STATUSES_IGNORE);
    MPI_Cancel(&st.subscribe_recv);
    MPI_Wait(&st.subscribe_recv, MPI_STATUS_IGNORE);
    MPI_Cancel(&st.status_recv);
    MPI_Wait(&st.status_recv, MPI_STATUS_IGNORE);

    printf("[Node %d] V3 reused neighbour cache in %lld checks; suppressed %lld duplicate alerts.\n",
           st.node.rank, st.cache_reuse_cycles, st.duplicate_alerts_saved);
    gather_stats(&st.stats, base_rank, NULL);
    free_node(&st.node);
    printf("[Node %d] Graceful shutdown complete.\n", st.node.rank);
    fflush(stdout);
}

/*
 * Function: handle_report
 * Purpose : Processes a received incident or refresh, computes alternatives
 *           from the base-station's recent-alert knowledge, and sends advice
 *           before doing file I/O, exactly as the V2 report handler does.
 */
void handle_report(const Config *cfg, const NodeReport *r, int source,
                   double start, double *last_report_time,
                   double *last_congested_time, Advice *advice_out,
                   MPI_Request *advice_send, FILE *log,
                   CommStats *stats, long long *recommendations)
{
    int j, recent_count, count;
    double now = global_time();
    Advice *advice = &advice_out[source];

    stats_record(stats, DELAY_REPORT, now - r->sent_at);
    record_report(r, now, last_report_time, last_congested_time);
    MPI_Wait(&advice_send[source], MPI_STATUS_IGNORE);
    count = find_alternative_nodes(cfg, r, last_report_time, last_congested_time,
                                   now, advice->nodes, &recent_count);
    advice->count = count;
    advice->recent_neighbour_reports = recent_count;
    advice->sent_at = global_time();
    MPI_Isend(advice, (int)sizeof(Advice), MPI_BYTE, source, TAG_ADVICE,
              MPI_COMM_WORLD, &advice_send[source]);
    stats_count_message(stats, (int)sizeof(Advice));
    *recommendations += count;

    fprintf(log, "\nREPORT received_at_sec=%.6f\n", now - start);
    fprintf(log, "  ALERT incident_time=%s sender_rank=%d sender_coord=(%d,%d) "
                 "sender_free_ports=%d port_threads=%d incoming_requests=%d "
                 "neighbour_messages_sent=%d neighbour_messages_received=%d "
                 "neighbour_bytes_sent=%d neighbour_bytes_received=%d "
                 "exchange_time_sec=%.9f\n",
            r->timestamp, r->rank, r->row, r->col, r->free_ports,
            r->port_threads, r->incoming_requests,
            r->neighbour_messages_sent, r->neighbour_messages_received,
            r->neighbour_bytes_sent, r->neighbour_bytes_received,
            r->neighbour_exchange_seconds);
    record_neighbours(log, r);
    fprintf(log, "  ADVICE to_node=%d recent_neighbour_alerts=%d count=%d nearest_nodes=",
            source, recent_count, count);
    if (count == 0) fprintf(log, "none");
    for (j = 0; j < count; j++)
        fprintf(log, "%s%d", (j ? "," : ""), advice->nodes[j]);
    fprintf(log, "\n");
}

/*
 * Function: run_base_station
 * Purpose : Listens for event-driven congestion reports and replies without
 *           global collectives. Sends TERM at the same rounds*interval time
 *           as V2 and participates in a nonblocking graceful-shutdown barrier.
 */
void run_base_station(const Config *cfg, int base_rank)
{
    int node_count = cfg->rows * cfg->cols;
    int i, ready, terminating = 0, done = 0;
    FILE *log = open_base_log(cfg, VERSION);
    double *last_report_time = NULL;
    double *last_congested_time = NULL;
    Advice *advice_out = NULL;
    MPI_Request *advice_send = NULL;
    MPI_Request *term_send = NULL;
    MPI_Request report_recv, barrier = MPI_REQUEST_NULL;
    MPI_Status status;
    NodeReport report_in;
    Termination term_out;
    CommStats stats, total;
    long long reports_received = 0, recommendations = 0;
    double start, end_time, simulation_seconds;
    memset(&stats, 0, sizeof(stats));

    last_report_time = (double *)malloc((size_t)node_count * sizeof(double));
    last_congested_time = (double *)malloc((size_t)node_count * sizeof(double));
    advice_out = (Advice *)calloc((size_t)node_count, sizeof(Advice));
    advice_send = (MPI_Request *)malloc((size_t)node_count * sizeof(MPI_Request));
    term_send = (MPI_Request *)malloc((size_t)node_count * sizeof(MPI_Request));
    if (!last_report_time || !last_congested_time || !advice_out ||
        !advice_send || !term_send)
    {
        fprintf(stderr, "[Base] Memory allocation error.\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    for (i = 0; i < node_count; i++)
    {
        last_report_time[i] = -1.0;
        last_congested_time[i] = -1.0;
        advice_send[i] = MPI_REQUEST_NULL;
        term_send[i] = MPI_REQUEST_NULL;
    }

    MPI_Irecv(&report_in, (int)sizeof(NodeReport), MPI_BYTE, MPI_ANY_SOURCE,
              TAG_REPORT, MPI_COMM_WORLD, &report_recv);
    start = global_time();
    end_time = start + cfg->rounds * (cfg->interval_ms / 1000.0);
    fprintf(log, "Note: V3 cache messages are subscriptions + changed status values.\n");
    fprintf(log, "Note: ongoing unchanged congestion generates one incident report, "
                 "then refreshes every %d ms.\n", V3_REPORT_REFRESH_MS);

    while (!done)
    {
        MPI_Test(&report_recv, &ready, &status);
        while (ready)
        {
            reports_received++;
            handle_report(cfg, &report_in, status.MPI_SOURCE, start,
                          last_report_time, last_congested_time,
                          advice_out, advice_send, log, &stats, &recommendations);
            MPI_Irecv(&report_in, (int)sizeof(NodeReport), MPI_BYTE,
                      MPI_ANY_SOURCE, TAG_REPORT, MPI_COMM_WORLD, &report_recv);
            MPI_Test(&report_recv, &ready, &status);
        }

        if (!terminating && global_time() >= end_time)
        {
            terminating = 1;
            term_out.sent_at = global_time();
            for (i = 0; i < node_count; i++)
            {
                MPI_Isend(&term_out, (int)sizeof(Termination), MPI_BYTE, i,
                          TAG_TERM, MPI_COMM_WORLD, &term_send[i]);
                stats_count_message(&stats, (int)sizeof(Termination));
            }
            MPI_Ibarrier(MPI_COMM_WORLD, &barrier);
            printf("[Base] V3 simulation time reached; TERM sent to %d nodes.\n",
                   node_count);
            fflush(stdout);
        }
        if (terminating) MPI_Test(&barrier, &done, MPI_STATUS_IGNORE);
        if (!done) sleep_microseconds(cfg->poll_us);
    }

    MPI_Cancel(&report_recv);
    MPI_Wait(&report_recv, MPI_STATUS_IGNORE);
    MPI_Waitall(node_count, advice_send, MPI_STATUSES_IGNORE);
    MPI_Waitall(node_count, term_send, MPI_STATUSES_IGNORE);
    simulation_seconds = global_time() - start;
    gather_stats(&stats, base_rank, &total);
    write_summary(log, cfg, VERSION, simulation_seconds, &total,
                  reports_received, recommendations);
    append_csv(cfg, VERSION, simulation_seconds, &total);
    fclose(log);

    printf("[Base] V3 finished after %.3f s. Reports: %lld. Log: %s\n",
           simulation_seconds, reports_received, cfg->log_path);
    fflush(stdout);
    free(term_send);
    free(advice_send);
    free(advice_out);
    free(last_congested_time);
    free(last_report_time);
}

/*
 * Function: main
 * Purpose : Initialises MPI/Cartesian topology using the existing common.c
 *           setup, runs the base station or EV node by rank, then finalises MPI.
 */
int main(int argc, char *argv[])
{
    Config cfg;
    MPI_Comm cart_comm;
    int base_rank;
    int role = wsn_init(&argc, &argv, VERSION, &cfg, &cart_comm, &base_rank);
    if (role == ROLE_INVALID) return EXIT_FAILURE;
    if (role == ROLE_BASE)
        run_base_station(&cfg, base_rank);
    else
    {
        run_charging_node(&cfg, cart_comm, base_rank);
        MPI_Comm_free(&cart_comm);
    }
    MPI_Finalize();
    return EXIT_SUCCESS;
}

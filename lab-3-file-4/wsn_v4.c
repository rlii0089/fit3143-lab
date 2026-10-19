//////////////////////////////////////////////////////////////////////////////////////
// wsn_v4.c - FIT3143 Lab 3, Version 4: non-blocking point-to-point, adaptive polling
// ----------------------------------------------------------------------------------
// Same messages and non-blocking calls as V3. One change: adaptive polling.
// V3 sleeps poll_us after every loop pass, so a reply that arrives during the
// sleep waits up to poll_us before it is noticed - while V2's blocking MPI_Recv
// notices it at once (but burns a CPU core while waiting). V4 only sleeps when
// no message is due, and checks continuously (no sleep) while one is:
//  * replies to this node's own neighbour request,
//  * advice from the base station after a report,
//  * TERM, once this node's last cycle has ended (the base station sends it at
//    the end of the same rounds x interval_ms timeline),
//  * the shutdown barrier; the base station also stops sleeping once it has
//    sent TERM.
// Idle nodes still sleep poll_us, so CPU use stays close to V3's.
//
// Only the main thread calls MPI (MPI_THREAD_FUNNELED); the OpenMP port threads
// only touch the shared port array.
// Example: mpirun -np 10 ./wsn_v4 3 3 --ports 6 --rounds 5 --scenario corner
//////////////////////////////////////////////////////////////////////////////////////

#include "common.h"

#define VERSION "V4"

typedef struct
{
    ChargingNode node;
    CommStats stats;
    int cycle;
    int terminating;               // TERM received: start no new requests/reports
    int requests_serviced;         // Neighbour requests answered this cycle

    // Incoming neighbour requests: one receive is always posted (any neighbour).
    MPI_Request request_recv;
    NbrRequest request_in;

    // Replies to neighbours, one slot per direction. A send buffer must stay
    // untouched until its non-blocking send completes.
    MPI_Request response_send[NUM_NEIGHBOURS];
    NbrResponse response_out[NUM_NEIGHBOURS];

    // This node's own request round (only while congested).
    int awaiting_neighbours;
    int responses_pending;
    double request_start;
    NbrRequest request_out;
    MPI_Request request_send[NUM_NEIGHBOURS];
    MPI_Request response_recv[NUM_NEIGHBOURS];
    NbrResponse response_in[NUM_NEIGHBOURS];

    // Report to the base station and the advice that comes back.
    int awaiting_advice;
    NodeReport report;
    MPI_Request report_send;
    Advice advice_in;
    MPI_Request advice_recv;

    // Termination message from the base station.
    Termination term_in;
    MPI_Request term_recv;
} NodeState;

/*
 * Function: direction_of
 * Purpose : Maps a neighbour's Cartesian rank to TOP/BOTTOM/LEFT/RIGHT.
 */
int direction_of(const ChargingNode *node, int rank)
{
    int i;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
        if (node->neighbours[i] == rank) return i;
    fprintf(stderr, "[Node %d] Message from non-neighbour %d.\n", node->rank, rank);
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    return -1;
}

/*
 * Function: service_neighbour_requests
 * Purpose : Answers every neighbour request that has arrived with this node's
 *           current free-port count (MPI_Isend), re-posting the receive each time.
 *           Called on every loop pass, including while this node is itself
 *           waiting for replies or advice - this is what prevents deadlock.
 */
void service_neighbour_requests(NodeState *st, MPI_Comm cart_comm)
{
    int flag, dir;
    MPI_Status status;

    MPI_Test(&st->request_recv, &flag, &status);
    while (flag)
    {
        stats_record(&st->stats, DELAY_NBR_REQ, global_time() - st->request_in.sent_at);
        dir = direction_of(&st->node, status.MPI_SOURCE);

        // The previous reply to this neighbour has already been received (it
        // only asks again after getting it), so this wait returns immediately.
        MPI_Wait(&st->response_send[dir], MPI_STATUS_IGNORE);
        st->response_out[dir].free_ports = st->node.free_ports;
        st->response_out[dir].sent_at = global_time();
        MPI_Isend(&st->response_out[dir], (int)sizeof(NbrResponse), MPI_BYTE,
                  status.MPI_SOURCE, TAG_NBR_RESP, cart_comm, &st->response_send[dir]);
        stats_count_message(&st->stats, (int)sizeof(NbrResponse));
        st->requests_serviced++;

        MPI_Irecv(&st->request_in, (int)sizeof(NbrRequest), MPI_BYTE, MPI_ANY_SOURCE,
                  TAG_NBR_REQ, cart_comm, &st->request_recv);
        MPI_Test(&st->request_recv, &flag, &status);
    }
}

/*
 * Function: send_report
 * Purpose : Builds the congestion report (all neighbours full) and sends it to
 *           the base station with MPI_Isend; the advice receive is posted first.
 */
void send_report(NodeState *st, const Config *cfg, int base_rank, double exchange_seconds)
{
    NodeReport *r = &st->report;
    int i, neighbours = 0;

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
    r->neighbour_exchange_seconds = exchange_seconds;
    current_timestamp(r->timestamp, TIME_LEN);   // When the incident was discovered

    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        r->neighbours[i] = st->node.neighbours[i];
        r->neighbour_free[i] = -1;
        if (st->node.neighbours[i] == MPI_PROC_NULL) continue;
        r->neighbour_free[i] = st->response_in[i].free_ports;
        r->requested[i] = 1;
        neighbours++;
    }
    // One request out and one response back per neighbour.
    r->neighbour_messages_sent = neighbours;
    r->neighbour_messages_received = neighbours;
    r->neighbour_bytes_sent = neighbours * (int)sizeof(NbrRequest);
    r->neighbour_bytes_received = neighbours * (int)sizeof(NbrResponse);

    MPI_Irecv(&st->advice_in, (int)sizeof(Advice), MPI_BYTE, base_rank, TAG_ADVICE,
              MPI_COMM_WORLD, &st->advice_recv);
    r->sent_at = global_time();
    MPI_Isend(r, (int)sizeof(NodeReport), MPI_BYTE, base_rank, TAG_REPORT,
              MPI_COMM_WORLD, &st->report_send);
    stats_count_message(&st->stats, (int)sizeof(NodeReport));
    st->stats.alerts++;
    st->awaiting_advice = 1;
}

/*
 * Function: finish_neighbour_round
 * Purpose : Called once every neighbour has replied. Redirects EVs to the best
 *           free neighbour, or (if all are full) reports to the base station.
 */
void finish_neighbour_round(NodeState *st, const Config *cfg, int base_rank)
{
    int i, redirect, neighbour_free[NUM_NEIGHBOURS];
    double exchange_seconds = global_time() - st->request_start;

    // One sample per completed exchange (until ALL neighbours replied), exactly
    // what V1 measures around its two neighbour collectives.
    stats_record(&st->stats, DELAY_NBR_RTT, exchange_seconds);

    // Our requests were all received (each neighbour replied), so these complete.
    MPI_Waitall(NUM_NEIGHBOURS, st->request_send, MPI_STATUSES_IGNORE);
    st->awaiting_neighbours = 0;

    for (i = 0; i < NUM_NEIGHBOURS; i++)
        neighbour_free[i] = st->response_in[i].free_ports;
    redirect = choose_redirect(cfg, st->node.neighbours, neighbour_free);

    if (redirect >= 0)
    {
        st->stats.redirects++;
        printf("[Cycle %d][Node %d] %d free ports; redirect EVs to neighbour %d.\n",
               st->cycle, st->node.rank, st->node.free_ports, redirect);
        fflush(stdout);
    }
    else if (!st->terminating)   // No new reports once the simulation has ended
        send_report(st, cfg, base_rank, exchange_seconds);
}

/*
 * Function: start_neighbour_round
 * Purpose : A congested node asks every neighbour for its availability. The
 *           response receives are posted BEFORE the requests are sent.
 */
void start_neighbour_round(NodeState *st, const Config *cfg, MPI_Comm cart_comm,
                           int base_rank)
{
    int i;
    st->request_start = global_time();
    st->request_out.sent_at = st->request_start;
    st->responses_pending = 0;

    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        int neighbour = st->node.neighbours[i];
        st->request_send[i] = MPI_REQUEST_NULL;
        st->response_recv[i] = MPI_REQUEST_NULL;
        st->response_in[i].free_ports = -1;
        if (neighbour == MPI_PROC_NULL) continue;

        MPI_Irecv(&st->response_in[i], (int)sizeof(NbrResponse), MPI_BYTE, neighbour,
                  TAG_NBR_RESP, cart_comm, &st->response_recv[i]);
        MPI_Isend(&st->request_out, (int)sizeof(NbrRequest), MPI_BYTE, neighbour,
                  TAG_NBR_REQ, cart_comm, &st->request_send[i]);
        stats_count_message(&st->stats, (int)sizeof(NbrRequest));
        st->responses_pending++;
    }

    st->awaiting_neighbours = 1;
    if (st->responses_pending == 0)       // 1x1 grid: no neighbours to ask
        finish_neighbour_round(st, cfg, base_rank);
}

/*
 * Function: check_neighbour_responses
 * Purpose : Collects whichever neighbour replies have arrived (MPI_Testsome),
 *           recording each one's delay, without waiting for the rest.
 */
void check_neighbour_responses(NodeState *st, const Config *cfg, int base_rank)
{
    int outcount, k, indices[NUM_NEIGHBOURS];
    double now;

    MPI_Testsome(NUM_NEIGHBOURS, st->response_recv, &outcount, indices,
                 MPI_STATUSES_IGNORE);
    if (outcount == MPI_UNDEFINED || outcount == 0) return;

    now = global_time();
    for (k = 0; k < outcount; k++)
    {
        int i = indices[k];
        stats_record(&st->stats, DELAY_NBR_RESP, now - st->response_in[i].sent_at);
        st->responses_pending--;
    }
    if (st->responses_pending == 0)
        finish_neighbour_round(st, cfg, base_rank);
}

/*
 * Function: check_advice
 * Purpose : Receives the base station's nearest-node advice if it has arrived,
 *           and prints what would be forwarded to incoming EVs.
 */
void check_advice(NodeState *st)
{
    int flag, i;
    double now;

    MPI_Test(&st->advice_recv, &flag, MPI_STATUS_IGNORE);
    if (!flag) return;

    now = global_time();
    stats_record(&st->stats, DELAY_ADVICE, now - st->advice_in.sent_at);
    stats_record(&st->stats, DELAY_REPORT_RTT, now - st->report.sent_at);
    MPI_Wait(&st->report_send, MPI_STATUS_IGNORE);   // Already received by the base
    st->awaiting_advice = 0;

    printf("[Cycle %d][Node %d] Congestion alert -> base station. ", st->cycle, st->node.rank);
    if (st->advice_in.count == 0)
        printf("No available nearby nodes.\n");
    else
    {
        printf("Nearest available node(s) for EVs:");
        for (i = 0; i < st->advice_in.count; i++) printf(" %d", st->advice_in.nodes[i]);
        printf("\n");
    }
    fflush(stdout);
}

/*
 * Function: message_due
 * Purpose : Adaptive polling: true while this node knows a message is on its
 *           way (neighbour replies, advice, TERM after its last cycle, or the
 *           shutdown barrier), so the event loop checks again without sleeping.
 */
int message_due(const NodeState *st, const Config *cfg, int in_barrier, double next_cycle)
{
    if (st->awaiting_neighbours || st->awaiting_advice || in_barrier) return 1;
    return !st->terminating && st->cycle >= cfg->rounds && global_time() >= next_cycle;
}

/*
 * Function: run_charging_node
 * Purpose : Runs Task 1 as an event loop. Each pass: update ports when the
 *           node's own cycle is due, then make progress on every outstanding
 *           message without blocking. Sleeps poll_us between passes only when
 *           no message is due, so idle nodes do not burn a CPU core.
 */
void run_charging_node(const Config *cfg, MPI_Comm cart_comm, int base_rank)
{
    NodeState st;
    MPI_Request barrier = MPI_REQUEST_NULL;
    int i, flag, in_barrier = 0, done = 0;
    double next_cycle;

    memset(&st, 0, sizeof(st));
    initialise_node(&st.node, cfg, cart_comm);
    print_node_topology(&st.node, cfg);
    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        st.response_send[i] = MPI_REQUEST_NULL;
        st.request_send[i] = MPI_REQUEST_NULL;
        st.response_recv[i] = MPI_REQUEST_NULL;
    }
    st.report_send = MPI_REQUEST_NULL;
    st.advice_recv = MPI_REQUEST_NULL;

    // Always ready for a neighbour's request and for the base station's TERM.
    MPI_Irecv(&st.request_in, (int)sizeof(NbrRequest), MPI_BYTE, MPI_ANY_SOURCE,
              TAG_NBR_REQ, cart_comm, &st.request_recv);
    MPI_Irecv(&st.term_in, (int)sizeof(Termination), MPI_BYTE, base_rank, TAG_TERM,
              MPI_COMM_WORLD, &st.term_recv);

    next_cycle = global_time();
    while (!done)
    {
        double now = global_time();
        // Exactly --rounds cycles (same workload as V1); then just keep
        // servicing neighbours until TERM arrives.
        if (!st.terminating && st.cycle < cfg->rounds && now >= next_cycle)
        {
            st.cycle++;
            st.stats.cycles++;
            st.requests_serviced = 0;
            update_charging_ports(&st.node, cfg, st.cycle);
            next_cycle += cfg->interval_ms / 1000.0;
            if (next_cycle < now) next_cycle = now;   // Fell behind: don't burst

            if (st.node.free_ports < cfg->threshold &&
                !st.awaiting_neighbours && !st.awaiting_advice)
                start_neighbour_round(&st, cfg, cart_comm, base_rank);
        }

        service_neighbour_requests(&st, cart_comm);
        if (st.awaiting_neighbours) check_neighbour_responses(&st, cfg, base_rank);
        if (st.awaiting_advice) check_advice(&st);

        if (!st.terminating)
        {
            MPI_Test(&st.term_recv, &flag, MPI_STATUS_IGNORE);
            if (flag)
            {
                st.terminating = 1;
                stats_record(&st.stats, DELAY_TERM, global_time() - st.term_in.sent_at);
            }
        }

        // Join the shutdown barrier only once nothing of ours is in flight; keep
        // servicing neighbours until EVERY process has joined.
        if (st.terminating && !in_barrier && !st.awaiting_neighbours && !st.awaiting_advice)
        {
            MPI_Ibarrier(MPI_COMM_WORLD, &barrier);
            in_barrier = 1;
        }
        if (in_barrier) MPI_Test(&barrier, &done, MPI_STATUS_IGNORE);
        if (!done && !message_due(&st, cfg, in_barrier, next_cycle))
            sleep_microseconds(cfg->poll_us);
    }

    // Every request/reply has completed, so the standing receive is unmatched:
    // cancel it, and confirm every reply send has finished.
    MPI_Cancel(&st.request_recv);
    MPI_Wait(&st.request_recv, MPI_STATUS_IGNORE);
    MPI_Waitall(NUM_NEIGHBOURS, st.response_send, MPI_STATUSES_IGNORE);

    gather_stats(&st.stats, base_rank, NULL);
    free_node(&st.node);
    printf("[Node %d] Graceful shutdown complete.\n", st.node.rank);
    fflush(stdout);
}

/*
 * Function: handle_report
 * Purpose : Processes one congestion report: updates the base station's
 *           knowledge, computes the nearest available nodes, sends the advice
 *           straight away (MPI_Isend), then logs the incident.
 */
void handle_report(const Config *cfg, const NodeReport *r, int source, double start,
                   double *last_report_time, double *last_congested_time,
                   Advice *advice_out, MPI_Request *advice_send, FILE *log,
                   CommStats *stats, long long *recommendations)
{
    int j, recent_count, count;
    double now = global_time();
    Advice *advice = &advice_out[source];

    stats_record(stats, DELAY_REPORT, now - r->sent_at);
    record_report(r, now, last_report_time, last_congested_time);

    // This node's previous advice was received before it could report again.
    MPI_Wait(&advice_send[source], MPI_STATUS_IGNORE);
    count = find_alternative_nodes(cfg, r, last_report_time, last_congested_time, now,
                                   advice->nodes, &recent_count);
    advice->count = count;
    advice->recent_neighbour_reports = recent_count;
    advice->sent_at = global_time();
    MPI_Isend(advice, (int)sizeof(Advice), MPI_BYTE, source, TAG_ADVICE, MPI_COMM_WORLD,
              &advice_send[source]);
    stats_count_message(stats, (int)sizeof(Advice));
    *recommendations += count;

    // Logged after the advice is on its way, so file I/O does not delay the node.
    fprintf(log, "\nREPORT received_at_sec=%.6f\n", now - start);
    fprintf(log, "  ALERT incident_time=%s sender_rank=%d sender_coord=(%d,%d) "
                 "sender_free_ports=%d port_threads=%d incoming_requests=%d "
                 "neighbour_messages_sent=%d neighbour_messages_received=%d "
                 "neighbour_bytes_sent=%d neighbour_bytes_received=%d "
                 "exchange_time_sec=%.9f\n",
            r->timestamp, r->rank, r->row, r->col, r->free_ports, r->port_threads,
            r->incoming_requests, r->neighbour_messages_sent,
            r->neighbour_messages_received, r->neighbour_bytes_sent,
            r->neighbour_bytes_received, r->neighbour_exchange_seconds);
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
 * Purpose : Runs Task 2 using exactly one MPI process and one MPI-calling thread.
 *           Periodically polls for reports (MPI_Test on a pre-posted MPI_Irecv),
 *           answers each straight away, and after rounds x interval_ms sends TERM
 *           to every node, then shuts down gracefully via MPI_Ibarrier (without
 *           sleeping, as every node is about to join it).
 */
void run_base_station(const Config *cfg, int base_rank)
{
    int node_count = cfg->rows * cfg->cols;
    int i, flag, terminating = 0, done = 0;
    FILE *log = open_base_log(cfg, VERSION);
    double *last_report_time = NULL;     // When each node last sent an alert report
    double *last_congested_time = NULL;  // When a report last showed a node congested
    Advice *advice_out = NULL;           // One send buffer per node
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
    if (last_report_time == NULL || last_congested_time == NULL || advice_out == NULL ||
        advice_send == NULL || term_send == NULL)
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

    // A report from any node can be received at any time.
    MPI_Irecv(&report_in, (int)sizeof(NodeReport), MPI_BYTE, MPI_ANY_SOURCE, TAG_REPORT,
              MPI_COMM_WORLD, &report_recv);
    start = global_time();
    end_time = start + cfg->rounds * (cfg->interval_ms / 1000.0);

    while (!done)
    {
        MPI_Test(&report_recv, &flag, &status);
        while (flag)
        {
            reports_received++;
            handle_report(cfg, &report_in, status.MPI_SOURCE, start, last_report_time,
                          last_congested_time, advice_out, advice_send, log, &stats,
                          &recommendations);
            MPI_Irecv(&report_in, (int)sizeof(NodeReport), MPI_BYTE, MPI_ANY_SOURCE,
                      TAG_REPORT, MPI_COMM_WORLD, &report_recv);
            MPI_Test(&report_recv, &flag, &status);
        }

        if (!terminating && global_time() >= end_time)
        {
            // TERM to every node (one shared, read-only buffer), then join the
            // barrier - but keep answering reports sent before TERM arrived.
            terminating = 1;
            term_out.sent_at = global_time();
            for (i = 0; i < node_count; i++)
            {
                MPI_Isend(&term_out, (int)sizeof(Termination), MPI_BYTE, i, TAG_TERM,
                          MPI_COMM_WORLD, &term_send[i]);
                stats_count_message(&stats, (int)sizeof(Termination));
            }
            MPI_Ibarrier(MPI_COMM_WORLD, &barrier);
            printf("[Base] Simulation time reached; TERM sent to %d nodes.\n", node_count);
            fflush(stdout);
        }
        if (terminating) MPI_Test(&barrier, &done, MPI_STATUS_IGNORE);
        if (!done && !terminating) sleep_microseconds(cfg->poll_us);
    }

    // Every node has finished (barrier), so no report can still arrive.
    MPI_Cancel(&report_recv);
    MPI_Wait(&report_recv, MPI_STATUS_IGNORE);
    MPI_Waitall(node_count, advice_send, MPI_STATUSES_IGNORE);
    MPI_Waitall(node_count, term_send, MPI_STATUSES_IGNORE);
    simulation_seconds = global_time() - start;

    gather_stats(&stats, base_rank, &total);
    write_summary(log, cfg, VERSION, simulation_seconds, &total, reports_received,
                  recommendations);
    append_csv(cfg, VERSION, simulation_seconds, &total);
    fclose(log);

    printf("[Base] Finished after %.3f s. Reports received: %lld. Log: %s\n",
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
 * Purpose : Sets up MPI and the Cartesian topology (common.c), then runs Task 1
 *           or Task 2 by MPI rank, and finalises MPI cleanly.
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

//////////////////////////////////////////////////////////////////////////////////////
// common.c - FIT3143 Lab 3 (Tasks 1 and 2): code shared by every version
// ----------------------------------------------------------------------------------
// See common.h. Nothing in this file decides HOW messages are passed; that is
// what distinguishes each version (wsn_v1.c, wsn_v2.c, ...).
//////////////////////////////////////////////////////////////////////////////////////

#include "common.h"

const char *DELAY_NAMES[NUM_DELAY_TYPES] = {
    "nbr_req", "nbr_resp", "nbr_rtt", "report", "advice", "report_rtt", "term"
};

static double clock_offset = 0.0;  // This process's clock minus the base station's

/*
 * Function: global_time
 * Purpose : Current time on the base station's clock (seconds). Used for every
 *           timestamp so one-way delays between two processes are meaningful.
 */
double global_time(void)
{
    return MPI_Wtime() + clock_offset;
}

/*
 * Function: synchronise_clocks
 * Purpose : Cristian's algorithm. Each node pings the base station several times;
 *           the base replies with its own time t_base. For the ping with the
 *           smallest round trip, offset = t_base - (t_send + t_receive) / 2, with
 *           an error of at most half that round trip. Runs once at start-up,
 *           before any delay is measured.
 */
static void synchronise_clocks(int world_rank, int world_size, int base_rank)
{
    int node, k;
    double base_time, ping = 0.0;

    if (world_rank == base_rank)
    {
        for (node = 0; node < world_size; node++)
        {
            if (node == base_rank) continue;
            for (k = 0; k < CLOCK_SYNC_ROUNDS; k++)
            {
                MPI_Recv(&ping, 1, MPI_DOUBLE, node, TAG_CLOCK_SYNC, MPI_COMM_WORLD,
                         MPI_STATUS_IGNORE);
                base_time = MPI_Wtime();
                MPI_Send(&base_time, 1, MPI_DOUBLE, node, TAG_CLOCK_SYNC, MPI_COMM_WORLD);
            }
        }
        return;
    }

    double best_round_trip = -1.0;
    for (k = 0; k < CLOCK_SYNC_ROUNDS; k++)
    {
        double sent = MPI_Wtime(), received, round_trip;
        MPI_Send(&sent, 1, MPI_DOUBLE, base_rank, TAG_CLOCK_SYNC, MPI_COMM_WORLD);
        MPI_Recv(&base_time, 1, MPI_DOUBLE, base_rank, TAG_CLOCK_SYNC, MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);
        received = MPI_Wtime();
        round_trip = received - sent;
        if (best_round_trip < 0.0 || round_trip < best_round_trip)
        {
            best_round_trip = round_trip;
            clock_offset = base_time - (sent + received) / 2.0;
        }
    }
}

/*
 * Function: print_usage
 * Purpose : Prints the command-line options and an example of launching the program.
 *           The required first two arguments are the Cartesian grid dimensions.
 */
static void print_usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s ROWS COLS [options]\n"
        "  --ports K              Ports (= OpenMP threads) per station, max %d (default: 6)\n"
        "  --threshold T          Congested when free ports < T (default: 1)\n"
        "  --rounds R             Number of simulation rounds (default: 10)\n"
        "  --interval-ms MS       Delay between rounds in ms (default: 1000)\n"
        "  --window-ms MS         Recent-report lookup window in ms (default: 30000)\n"
        "  --poll-us US           Polling interval of event-driven versions (default: 100)\n"
        "  --arrival P            Free-to-occupied probability, percent (default: 45)\n"
        "  --departure P          Occupied-to-free probability, percent (default: 35)\n"
        "  --scenario NAME        random | demo | corner | full (default: random)\n"
        "  --seed N               Random seed (default: 12345)\n"
        "  --log PATH             Base-station output file (default: base_station.log)\n"
        "  --csv PATH             Append one row of delay results to a CSV file\n"
        "Example: mpirun -np 10 %s 3 3 --ports 6 --rounds 5 --scenario demo\n",
        program, MAX_PORTS, program);
}

/*
 * Function: parse_integer
 * Purpose : Converts a string to an integer, rejecting non-numeric input,
 *           integer overflow, and numbers outside the requested range.
 * Returns : 1 when valid; 0 otherwise.
 */
static int parse_integer(const char *text, int minimum, int maximum, int *value)
{
    char *end = NULL;
    long number;
    errno = 0;
    number = strtol(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || *end != '\0' ||
        number < minimum || number > maximum)
        return 0;
    *value = (int)number;
    return 1;
}

/*
 * Function: parse_path
 * Purpose : Copies a non-empty file path option into a fixed-size buffer.
 * Returns : 1 when valid; 0 otherwise.
 */
static int parse_path(const char *text, char *path)
{
    if (strlen(text) >= LOG_PATH_LEN || text[0] == '\0') return 0;
    snprintf(path, LOG_PATH_LEN, "%s", text);
    return 1;
}

/*
 * Function: parse_config
 * Purpose : Sets default simulation options, then reads and validates optional
 *           command-line parameters. Only the base-station rank calls this.
 * Returns : 1 for valid arguments; 0 for invalid arguments.
 */
static int parse_config(int argc, char *argv[], Config *cfg)
{
    int i;
    memset(cfg, 0, sizeof(*cfg));
    cfg->ports = 6;
    cfg->threshold = 1;
    cfg->rounds = 10;
    cfg->interval_ms = 1000;
    cfg->window_ms = 30000;
    cfg->poll_us = 100;
    cfg->arrival_percent = 45;
    cfg->departure_percent = 35;
    cfg->seed = 12345;
    cfg->scenario = SCENARIO_RANDOM;
    snprintf(cfg->log_path, LOG_PATH_LEN, "base_station.log");

    if (argc < 3 || !parse_integer(argv[1], 1, 10000, &cfg->rows) ||
        !parse_integer(argv[2], 1, 10000, &cfg->cols))
        return 0;

    for (i = 3; i < argc; i += 2)
    {
        const char *option = argv[i];
        const char *value;
        if (i + 1 >= argc)
            return 0;
        value = argv[i + 1];

        if (strcmp(option, "--ports") == 0)
        {
            if (!parse_integer(value, 1, MAX_PORTS, &cfg->ports)) return 0;
        }
        else if (strcmp(option, "--threshold") == 0)
        {
            if (!parse_integer(value, 1, 10000, &cfg->threshold)) return 0;
        }
        else if (strcmp(option, "--rounds") == 0)
        {
            if (!parse_integer(value, 1, 100000, &cfg->rounds)) return 0;
        }
        else if (strcmp(option, "--interval-ms") == 0)
        {
            if (!parse_integer(value, 0, 3600000, &cfg->interval_ms)) return 0;
        }
        else if (strcmp(option, "--window-ms") == 0)
        {
            if (!parse_integer(value, 0, 86400000, &cfg->window_ms)) return 0;
        }
        else if (strcmp(option, "--poll-us") == 0)
        {
            if (!parse_integer(value, 0, 1000000, &cfg->poll_us)) return 0;
        }
        else if (strcmp(option, "--arrival") == 0)
        {
            if (!parse_integer(value, 0, 100, &cfg->arrival_percent)) return 0;
        }
        else if (strcmp(option, "--departure") == 0)
        {
            if (!parse_integer(value, 0, 100, &cfg->departure_percent)) return 0;
        }
        else if (strcmp(option, "--seed") == 0)
        {
            if (!parse_integer(value, 0, INT_MAX, &cfg->seed)) return 0;
        }
        else if (strcmp(option, "--scenario") == 0)
        {
            if (strcmp(value, "random") == 0) cfg->scenario = SCENARIO_RANDOM;
            else if (strcmp(value, "demo") == 0) cfg->scenario = SCENARIO_DEMO;
            else if (strcmp(value, "full") == 0) cfg->scenario = SCENARIO_FULL;
            else if (strcmp(value, "corner") == 0) cfg->scenario = SCENARIO_CORNER;
            else return 0;
        }
        else if (strcmp(option, "--log") == 0)
        {
            if (!parse_path(value, cfg->log_path)) return 0;
        }
        else if (strcmp(option, "--csv") == 0)
        {
            if (!parse_path(value, cfg->csv_path)) return 0;
        }
        else return 0;
    }

    if (cfg->threshold > cfg->ports) return 0;
    if ((long long)cfg->rows * cfg->cols > INT_MAX - 1) return 0;
    return 1;
}

/*
 * Function: wsn_init
 * Purpose : Initialises MPI with FUNNELED thread support, validates that the
 *           number of processes equals (rows * columns + 1), shares the
 *           configuration, and creates a separate Cartesian communicator for the
 *           charging nodes. The last world rank is the base station.
 * Returns : ROLE_BASE, ROLE_NODE, or ROLE_INVALID (MPI already finalised).
 */
int wsn_init(int *argc, char ***argv, const char *version, Config *cfg,
             MPI_Comm *cart_comm, int *base_rank)
{
    int world_rank, world_size, provided;
    int valid = 0;
    int dims[NUM_DIMS], wrap_around[NUM_DIMS] = {0, 0};
    MPI_Comm nodes_comm = MPI_COMM_NULL;

    *cart_comm = MPI_COMM_NULL;
    MPI_Init_thread(argc, argv, MPI_THREAD_FUNNELED, &provided);
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    *base_rank = world_size - 1;

    if (provided < MPI_THREAD_FUNNELED)
    {
        if (world_rank == *base_rank)
            fprintf(stderr, "MPI implementation does not support MPI_THREAD_FUNNELED.\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    if (world_rank == *base_rank)
    {
        valid = parse_config(*argc, *argv, cfg);
        if (valid && ((long long)cfg->rows * cfg->cols + 1 != world_size))
        {
            fprintf(stderr, "Expected %lld MPI processes, but found %d.\n",
                    (long long)cfg->rows * cfg->cols + 1, world_size);
            valid = 0;
        }
        if (!valid) print_usage((*argv)[0]);
    }
    MPI_Bcast(&valid, 1, MPI_INT, *base_rank, MPI_COMM_WORLD);
    if (!valid)
    {
        MPI_Finalize();
        return ROLE_INVALID;
    }
    MPI_Bcast(cfg, (int)sizeof(*cfg), MPI_BYTE, *base_rank, MPI_COMM_WORLD);
    synchronise_clocks(world_rank, world_size, *base_rank);

    // Separate charging-node ranks from the base-station rank. Only charging
    // ranks are permitted in the Cartesian grid communicator.
    MPI_Comm_split(MPI_COMM_WORLD,
                   (world_rank == *base_rank ? MPI_UNDEFINED : 0),
                   world_rank, &nodes_comm);

    if (world_rank == *base_rank)
    {
        printf("[Base] %s: grid %d x %d, ports/node=%d, MPI processes=%d.\n",
               version, cfg->rows, cfg->cols, cfg->ports, world_size);
        fflush(stdout);
        MPI_Barrier(MPI_COMM_WORLD);       // Matches the nodes' start barrier below
        return ROLE_BASE;
    }

    dims[0] = cfg->rows;
    dims[1] = cfg->cols;
    // reorder = 0 keeps Cartesian rank == world rank, so the base station can
    // address a node by the rank in its report.
    MPI_Cart_create(nodes_comm, NUM_DIMS, dims, wrap_around, 0, cart_comm);
    if (*cart_comm == MPI_COMM_NULL)
    {
        fprintf(stderr, "[Rank %d] Failed to create MPI Cartesian topology.\n", world_rank);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    MPI_Comm_free(&nodes_comm);    // The Cartesian communicator does not depend on it

    // Warm-up: the first message between two processes (especially on different
    // machines) pays a one-off connection set-up. Exchange one value with every
    // neighbour now (node <-> base pairs were already used by the clock sync),
    // so that cost is not counted as communication delay in any version.
    {
        int warm_out = world_rank, warm_in[NUM_NEIGHBOURS];
        MPI_Neighbor_allgather(&warm_out, 1, MPI_INT, warm_in, 1, MPI_INT, *cart_comm);
    }
    // Everyone starts the simulation together.
    MPI_Barrier(MPI_COMM_WORLD);
    return ROLE_NODE;
}

/*
 * Function: sleep_milliseconds / sleep_microseconds
 * Purpose : Pause without busy-waiting. Retries if a signal interrupts nanosleep().
 */
void sleep_microseconds(int microseconds)
{
    struct timespec delay;
    if (microseconds <= 0) return;
    delay.tv_sec = microseconds / 1000000;
    delay.tv_nsec = (long)(microseconds % 1000000) * 1000L;
    while (nanosleep(&delay, &delay) == -1 && errno == EINTR) { }
}

void sleep_milliseconds(int milliseconds)
{
    struct timespec delay;
    if (milliseconds <= 0) return;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) == -1 && errno == EINTR) { }
}

/*
 * Function: current_timestamp
 * Purpose : Formats the local wall-clock date and time of a congestion event.
 */
void current_timestamp(char *buffer, size_t capacity)
{
    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    strftime(buffer, capacity, "%Y-%m-%d %H:%M:%S", &local_time);
}

/*
 * Function: next_random
 * Purpose : Generates a small pseudo-random value for a single charging port.
 *           Each port owns its own state, avoiding rand() races between threads.
 */
static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/*
 * Function: initialise_node
 * Purpose : Allocates the port-status shared array and per-port random states;
 *           also obtains the node's coordinates and immediate Cartesian neighbours.
 */
void initialise_node(ChargingNode *node, const Config *cfg, MPI_Comm cart_comm)
{
    int i;
    memset(node, 0, sizeof(*node));
    MPI_Comm_rank(cart_comm, &node->rank);
    MPI_Cart_coords(cart_comm, node->rank, NUM_DIMS, node->coords);
    MPI_Cart_shift(cart_comm, 0, 1, &node->neighbours[TOP], &node->neighbours[BOTTOM]);
    MPI_Cart_shift(cart_comm, 1, 1, &node->neighbours[LEFT], &node->neighbours[RIGHT]);

    node->port_status = (int *)calloc((size_t)cfg->ports, sizeof(int));
    node->random_state = (uint32_t *)calloc((size_t)cfg->ports, sizeof(uint32_t));
    if (node->port_status == NULL || node->random_state == NULL)
    {
        fprintf(stderr, "[Node %d] Could not allocate charging-port arrays.\n", node->rank);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    for (i = 0; i < cfg->ports; i++)
    {
        uint32_t s = (uint32_t)cfg->seed ^
                     ((uint32_t)(node->rank + 1) * UINT32_C(747796405)) ^
                     ((uint32_t)(i + 1) * UINT32_C(2891336453));
        node->random_state[i] = (s == 0) ? UINT32_C(1) : s;
        node->port_status[i] = ((next_random(&node->random_state[i]) % 100U) < 60U);
    }

    // Stop the OpenMP runtime from silently giving us fewer threads than ports.
    omp_set_dynamic(0);

    // Warm-up: create the OpenMP thread team now, so its one-off start-up cost
    // is not counted inside the first round's communication timing.
    #pragma omp parallel num_threads(cfg->ports)
    { }
}

/*
 * Function: free_node
 * Purpose : Releases the node's shared port array and random states.
 */
void free_node(ChargingNode *node)
{
    free(node->port_status);
    free(node->random_state);
    node->port_status = NULL;
    node->random_state = NULL;
}

/*
 * Function: print_node_topology
 * Purpose : Shows the virtual topology is correct (-1 = no neighbour on that side).
 */
void print_node_topology(const ChargingNode *node, const Config *cfg)
{
    printf("[Node %d] coord=(%d,%d) neighbours top=%d bottom=%d left=%d right=%d ports=%d\n",
           node->rank, node->coords[0], node->coords[1],
           node->neighbours[TOP] < 0 ? -1 : node->neighbours[TOP],
           node->neighbours[BOTTOM] < 0 ? -1 : node->neighbours[BOTTOM],
           node->neighbours[LEFT] < 0 ? -1 : node->neighbours[LEFT],
           node->neighbours[RIGHT] < 0 ? -1 : node->neighbours[RIGHT],
           cfg->ports);
    fflush(stdout);
}

/*
 * Function: update_charging_ports
 * Purpose : Uses OpenMP to simulate arrivals/departures at all charging ports.
 *           Exactly one thread per port (k threads for k ports), so each thread
 *           updates one unique entry in the shared array. The reduction safely
 *           counts free ports after the updates complete.
 */
void update_charging_ports(ChargingNode *node, const Config *cfg, int round)
{
    int free_count = 0;
    int row = node->coords[0], col = node->coords[1];
    int distance_from_centre = abs(row - cfg->rows / 2) + abs(col - cfg->cols / 2);
    // demo:   centre node + its direct neighbours are full.
    // corner: Node 0 + its direct neighbours (Nodes 1, 3) are full, matching
    //         the specification's base-station example.
    int in_full_region = (cfg->scenario == SCENARIO_DEMO) ? (distance_from_centre <= 1)
                                                          : (row + col <= 1);

    #pragma omp parallel for num_threads(cfg->ports) reduction(+:free_count) schedule(static)
    for (int port = 0; port < cfg->ports; port++)
    {
        // Only thread 0 writes, so no race; read after the implicit barrier.
        if (omp_get_thread_num() == 0)
            node->port_threads = omp_get_num_threads();

        if (cfg->scenario == SCENARIO_FULL)
        {
            node->port_status[port] = 1;
        }
        else if (cfg->scenario == SCENARIO_DEMO || cfg->scenario == SCENARIO_CORNER)
        {
            // Full region stays full; farther nodes have spare ports.
            // Rotate the free port(s) to demonstrate the shared array changing.
            if (in_full_region)
                node->port_status[port] = 1;
            else
                node->port_status[port] = ((port + round) % cfg->ports >= cfg->threshold);
        }
        else
        {
            int change = (int)(next_random(&node->random_state[port]) % 100U);
            if (node->port_status[port] == 0 && change < cfg->arrival_percent)
                node->port_status[port] = 1; // An EV arrives.
            else if (node->port_status[port] == 1 && change < cfg->departure_percent)
                node->port_status[port] = 0; // An EV leaves.
        }
        if (node->port_status[port] == 0) free_count++;
    }
    node->free_ports = free_count;
}

/*
 * Function: choose_redirect
 * Purpose : Picks the non-congested neighbour with the most free ports (lowest
 *           rank on a tie) for a congested node to send incoming EVs to.
 * Returns : The neighbour's rank, or -1 if every neighbour is congested.
 */
int choose_redirect(const Config *cfg, const int *neighbours, const int *neighbour_free)
{
    int i, best_rank = -1, best_free = -1;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        if (neighbours[i] == MPI_PROC_NULL || neighbour_free[i] < cfg->threshold)
            continue;
        if (neighbour_free[i] > best_free ||
            (neighbour_free[i] == best_free && neighbours[i] < best_rank))
        {
            best_rank = neighbours[i];
            best_free = neighbour_free[i];
        }
    }
    return best_rank;
}

/*
 * Function: is_recent
 * Purpose : Returns 1 when an event (MPI_Wtime seconds, -1 = never) happened
 *           within the base station's predefined report window.
 */
int is_recent(double event_time, double now, const Config *cfg)
{
    return event_time >= 0.0 && (now - event_time) * 1000.0 <= (double)cfg->window_ms;
}

/*
 * Function: record_report
 * Purpose : Updates the base station's knowledge from one congestion report:
 *           when the node last reported, and that the reporter AND all of its
 *           neighbours were congested at that time.
 */
void record_report(const NodeReport *report, double now, double *last_report_time,
                   double *last_congested_time)
{
    int j;
    last_report_time[report->rank] = now;
    last_congested_time[report->rank] = now;
    for (j = 0; j < NUM_NEIGHBOURS; j++)
        if (report->neighbours[j] != MPI_PROC_NULL)
            last_congested_time[report->neighbours[j]] = now;
}

/*
 * Function: find_alternative_nodes
 * Purpose : Implements Task 2's information update mechanism using ONLY the
 *           congestion reports received by the base station:
 *           1. If EVERY direct neighbour of the reporter has itself reported
 *              within the window, its whole neighbourhood is known to be full,
 *              so there are no nodes available nearby (returns 0).
 *           2. Otherwise the candidates are the nodes two hops away (e.g. Node 0
 *              -> Nodes 2, 4, 6 in a 3x3 grid). A candidate is skipped if a
 *              report within the window showed it congested (either as the
 *              reporter or as one of a reporter's neighbours).
 *           3. ALL remaining candidates at the minimum Euclidean distance are
 *              returned, e.g. Node 4 (diagonal, sqrt 2) beats Nodes 2 and 6
 *              (straight, 2); if Node 4 is full, both 2 and 6 are reported.
 * Returns : Number of suggested nodes written to results (at most 8).
 */
int find_alternative_nodes(const Config *cfg, const NodeReport *report,
                           const double *last_report_time,
                           const double *last_congested_time,
                           double now, int *results, int *recent_count)
{
    int i, dr, dc, result_count = 0, neighbours = 0, min_distance_sq = INT_MAX;
    *recent_count = 0;

    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        int rank = report->neighbours[i];
        if (rank == MPI_PROC_NULL) continue;
        neighbours++;
        if (is_recent(last_report_time[rank], now, cfg))
            (*recent_count)++;
    }

    // The specification's rule: every adjacent node having reported recently
    // means the base station returns "no nodes available nearby".
    if (neighbours > 0 && *recent_count == neighbours)
        return 0;

    for (dr = -2; dr <= 2; dr++)
    {
        for (dc = -2; dc <= 2; dc++)
        {
            int row = report->row + dr, col = report->col + dc;
            int rank, distance_sq;
            if (abs(dr) + abs(dc) != 2) continue;  // only nodes two hops away
            if (row < 0 || row >= cfg->rows || col < 0 || col >= cfg->cols) continue;

            // Row-major Cartesian rank, as created by MPI_Cart_create (reorder = 0).
            rank = row * cfg->cols + col;
            if (is_recent(last_congested_time[rank], now, cfg)) continue;

            distance_sq = dr * dr + dc * dc;       // 2 = diagonal, 4 = straight
            if (distance_sq < min_distance_sq)
            {
                min_distance_sq = distance_sq;
                result_count = 0;
            }
            if (distance_sq == min_distance_sq)
                results[result_count++] = rank;
        }
    }
    return result_count;
}

/*
 * Function: record_neighbours
 * Purpose : Writes neighbour ranks and reported free-port availability to the
 *           base-station log; used for explaining congestion incidents.
 */
void record_neighbours(FILE *log, const NodeReport *report)
{
    static const char *names[NUM_NEIGHBOURS] = {"top", "bottom", "left", "right"};
    int i;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        if (report->neighbours[i] == MPI_PROC_NULL) continue;
        fprintf(log, "    %-6s node=%d free_ports=%d request=%d\n",
                names[i], report->neighbours[i], report->neighbour_free[i],
                report->requested[i]);
    }
}

/*
 * Function: open_base_log
 * Purpose : Opens the base-station log and writes the run configuration.
 */
FILE *open_base_log(const Config *cfg, const char *version)
{
    FILE *log = fopen(cfg->log_path, "w");
    if (log == NULL)
    {
        fprintf(stderr, "[Base] Could not open log file '%s'.\n", cfg->log_path);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    fprintf(log, "FIT3143 Lab 3 - Base Station Log (%s)\n", version);
    fprintf(log, "grid=%dx%d ports=%d threshold=%d rounds=%d interval_ms=%d "
                 "window_ms=%d poll_us=%d scenario=%d seed=%d\n",
            cfg->rows, cfg->cols, cfg->ports, cfg->threshold, cfg->rounds,
            cfg->interval_ms, cfg->window_ms, cfg->poll_us, cfg->scenario, cfg->seed);
    return log;
}

/*
 * Function: stats_record
 * Purpose : Adds one measured delay (seconds) of the given type.
 */
void stats_record(CommStats *stats, int type, double delay_seconds)
{
    stats->count[type]++;
    stats->total[type] += delay_seconds;
    if (delay_seconds > stats->max[type]) stats->max[type] = delay_seconds;
}

/*
 * Function: stats_count_message
 * Purpose : Counts one logical message (one destination) and its payload size.
 */
void stats_count_message(CommStats *stats, int bytes)
{
    stats->messages_sent++;
    stats->bytes_sent += bytes;
}

/*
 * Function: gather_stats
 * Purpose : Collective on MPI_COMM_WORLD, called by every process once the
 *           simulation is over (so it adds nothing to the measured traffic).
 *           The base station receives each process's statistics and returns
 *           their combined totals in *total (other ranks may pass NULL).
 */
void gather_stats(const CommStats *mine, int base_rank, CommStats *total)
{
    int world_rank, world_size, p, t;
    CommStats *all = NULL;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    if (world_rank == base_rank)
    {
        all = (CommStats *)malloc((size_t)world_size * sizeof(CommStats));
        if (all == NULL) MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    MPI_Gather(mine, (int)sizeof(CommStats), MPI_BYTE, all, (int)sizeof(CommStats),
               MPI_BYTE, base_rank, MPI_COMM_WORLD);
    if (world_rank != base_rank) return;

    memset(total, 0, sizeof(*total));
    for (p = 0; p < world_size; p++)
    {
        for (t = 0; t < NUM_DELAY_TYPES; t++)
        {
            total->count[t] += all[p].count[t];
            total->total[t] += all[p].total[t];
            if (all[p].max[t] > total->max[t]) total->max[t] = all[p].max[t];
        }
        total->messages_sent += all[p].messages_sent;
        total->bytes_sent += all[p].bytes_sent;
        total->cycles += all[p].cycles;
        total->redirects += all[p].redirects;
        total->alerts += all[p].alerts;
    }
    free(all);
}

/*
 * Function: write_summary
 * Purpose : Writes the key performance metrics and the average/maximum delay of
 *           every message type to the base-station log.
 */
void write_summary(FILE *log, const Config *cfg, const char *version,
                   double simulation_seconds, const CommStats *total,
                   long long reports_received, long long recommendations)
{
    int t;
    fprintf(log, "\n================ FINAL SUMMARY ================\n");
    fprintf(log, "version=%s grid=%dx%d nodes=%d\n", version, cfg->rows, cfg->cols,
            cfg->rows * cfg->cols);
    fprintf(log, "simulation_time_sec=%.6f\n", simulation_seconds);
    fprintf(log, "total_port_update_cycles=%lld\n", total->cycles);
    fprintf(log, "total_alerts_sent=%lld\n", total->alerts);
    fprintf(log, "total_reports_received=%lld\n", reports_received);
    fprintf(log, "total_redirects_to_neighbours=%lld\n", total->redirects);
    fprintf(log, "total_recommended_nodes=%lld\n", recommendations);
    fprintf(log, "total_messages_sent=%lld\n", total->messages_sent);
    fprintf(log, "total_bytes_sent=%lld\n", total->bytes_sent);
    fprintf(log, "communication delay (ms):\n");
    fprintf(log, "  %-11s %10s %12s %12s\n", "type", "count", "avg_ms", "max_ms");
    for (t = 0; t < NUM_DELAY_TYPES; t++)
    {
        double average = total->count[t] ? total->total[t] / total->count[t] : 0.0;
        fprintf(log, "  %-11s %10lld %12.6f %12.6f\n", DELAY_NAMES[t], total->count[t],
                average * 1000.0, total->max[t] * 1000.0);
    }
    fprintf(log, "graceful_shutdown=complete\n");
}

/*
 * Function: append_csv
 * Purpose : Appends one row (one simulation run) to the CSV file given by --csv,
 *           writing the header first if the file is new. Used for Task 3 graphs.
 */
void append_csv(const Config *cfg, const char *version, double simulation_seconds,
                const CommStats *total)
{
    int t;
    FILE *csv;
    if (cfg->csv_path[0] == '\0') return;

    csv = fopen(cfg->csv_path, "a");
    if (csv == NULL)
    {
        fprintf(stderr, "[Base] Could not open CSV file '%s'.\n", cfg->csv_path);
        return;
    }
    if (ftell(csv) == 0)
    {
        fprintf(csv, "version,rows,cols,nodes,ports,threshold,rounds,interval_ms,"
                     "window_ms,poll_us,sim_time_s,cycles,alerts,redirects,messages,bytes");
        for (t = 0; t < NUM_DELAY_TYPES; t++)
            fprintf(csv, ",%s_count,%s_avg_ms,%s_max_ms",
                    DELAY_NAMES[t], DELAY_NAMES[t], DELAY_NAMES[t]);
        fprintf(csv, "\n");
    }
    fprintf(csv, "%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.6f,%lld,%lld,%lld,%lld,%lld",
            version, cfg->rows, cfg->cols, cfg->rows * cfg->cols, cfg->ports,
            cfg->threshold, cfg->rounds, cfg->interval_ms, cfg->window_ms, cfg->poll_us,
            simulation_seconds, total->cycles, total->alerts, total->redirects,
            total->messages_sent, total->bytes_sent);
    for (t = 0; t < NUM_DELAY_TYPES; t++)
    {
        double average = total->count[t] ? total->total[t] / total->count[t] : 0.0;
        fprintf(csv, ",%lld,%.6f,%.6f", total->count[t], average * 1000.0,
                total->max[t] * 1000.0);
    }
    fprintf(csv, "\n");
    fclose(csv);
}

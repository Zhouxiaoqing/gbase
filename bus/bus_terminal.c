#include <assert.h>
#include "core/shm.h"
#include "core/atom.h"
#include "core/idtable.h"
#include "core/process_lock.h"
#include "core/rbuffer.h"
#include "bus/bus_channel.h"
#include "bus/bus_terminal.h"

// stored in share memory
typedef struct bus_t
{
    int32_t key;
    int16_t channel_key;
    size_t size;

    // terminals info
    atom_t terminal_version;
    int terminal_count;
    bus_addr_t terminals[BUS_MAX_TERMINAL_COUNT];

    // channel info
    atom_t channel_version;
    int channel_count;
    struct bus_channel_t channels[BUS_MAX_CHANNLE_COUNT];
} bus_t;

typedef struct bus_terminal_t
{
    bus_t* bus;

    bus_addr_t self;

    uint32_t local_terminal_version;
    uint32_t local_channel_version;

    struct process_lock_t* lock;

    struct idtable_t* send_channels;
    struct idtable_t* recv_channels;
} bus_terminal_t;

int32_t _bus_terminal_init_create(bus_terminal_t* bt, int16_t key)
{
    int32_t buskey = (key << 16);
    struct shm_t* shm = shm_create(buskey, sizeof(bus_t), 0);
    if (shm) {
        bt->bus = (bus_t*)shm_mem(shm);
        assert(bt->bus);

        bt->bus->key = buskey;
        bt->bus->channel_key = 0;
        bt->bus->size = sizeof(bus_t);

        // terminals info
        bt->bus->terminal_count = 1;
        bt->bus->terminals[0] = bt->self;
        atom_set(&bt->bus->terminal_version, 1);
        bt->local_terminal_version = bt->bus->terminal_version;

        // channels info
        bt->bus->channel_count = 0;
        atom_set(&bt->bus->channel_version, 1);
        bt->local_channel_version = bt->bus->channel_version;
        return 0;
    }
    return -1;
}

void _bus_terminal_update_channels(bus_terminal_t* bt)
{
    struct bus_terminal_channel_t* btc;
    int32_t i, ret;
    assert(bt && bt->bus);

    // load terminal channel info
    for (i = 0; i < bt->bus->channel_count; ++ i) {

        if (bt->bus->channels[i].from == bt->self && idtable_get(bt->send_channels,
            bt->bus->channels[i].to) == NULL) {
            btc = bus_terminal_channel_init(bt->bus->channels[i].shmkey,
                bt->bus->channels[i].from, bt->bus->channels[i].to,
                bt->bus->channels[i].channel_size, 1);
            assert(btc);
            ret = idtable_add(bt->send_channels, bt->bus->channels[i].to, btc);
            assert(0 == ret);

        } else if (bt->bus->channels[i].to == bt->self && idtable_get(bt->recv_channels,
            bt->bus->channels[i].from) == NULL) {
            btc = bus_terminal_channel_init(bt->bus->channels[i].shmkey,
                bt->bus->channels[i].from, bt->bus->channels[i].to,
                bt->bus->channels[i].channel_size, 1);
            assert(btc);
            ret = idtable_add(bt->recv_channels, bt->bus->channels[i].from, btc);
            assert(0 == ret);
        }
    }
}

int32_t _bus_terminal_init_attach(bus_terminal_t* bt, int16_t key)
{
    int32_t buskey = (key << 16);
    int32_t i;
    struct shm_t* shm;
    int32_t registered = -1;

    // attach exsit shm
    shm = shm_create(buskey, sizeof(bus_t), 1);
    if (!shm) {
        return -1;
    }
    bt->bus = (bus_t*)shm_mem(shm);

    // invalid shm data
    if (bt->bus->key != buskey || bt->bus->size != sizeof(bus_t)) {
        return -1;
    }

    // register terminal info
    for (i = 0; i < bt->bus->terminal_count; ++ i) {
        if (bt->bus->terminals[i] == bt->self) {
            registered = 0;
            break;
        }
    }
    if (registered != 0) {
        if (bt->bus->terminal_count >= BUS_MAX_TERMINAL_COUNT) {
            return -1;
        } else {
            bt->bus->terminals[bt->bus->terminal_count ++] = bt->self;
            atom_inc(&bt->bus->terminal_version);
        }
    }

    // load channels info
    _bus_terminal_update_channels(bt);

    // load terminal version info
    bt->local_terminal_version = bt->bus->terminal_version;
    bt->local_channel_version = bt->bus->channel_version;

    return 0;
}

bus_terminal_t* bus_terminal_init(int16_t key, bus_addr_t ba)
{
    bus_terminal_t* bt;

    // init bus terminal
    bt= (bus_terminal_t*)MALLOC(sizeof(*bt));
    if (!bt) return NULL;
    bt->bus = NULL;
    bt->self = ba;
    bt->local_terminal_version = 0;
    bt->local_channel_version = 0;
    bt->lock = process_lock_init(key);
    assert(bt->lock);
    bt->send_channels = idtable_init(BUS_MAX_TERMINAL_COUNT);
    assert(bt->send_channels);
    bt->recv_channels = idtable_init(BUS_MAX_TERMINAL_COUNT);
    assert(bt->recv_channels);

    // add lock
    process_lock_lock(bt->lock);

    // create shm
    if (_bus_terminal_init_create(bt, key) == 0) {
        process_lock_unlock(bt->lock);
        return bt;
    }

    // attach shm
    if (_bus_terminal_init_attach(bt, key) == 0) {
        process_lock_unlock(bt->lock);
        return bt;
    }

    // terminal init fail
    process_lock_unlock(bt->lock);
    FREE(bt);
    return NULL;
}

void bus_terminal_release(bus_terminal_t* bt)
{
    struct idtable_iterator_t* it;
    struct bus_terminal_channel_t* btc;
    if (bt) {
        // release send channels
        if (bt->send_channels) {
            it = idtable_iterator_init(bt->send_channels, 0);
            while (it && idtable_iterator_loop(it) == 0) {
                btc = (struct bus_terminal_channel_t*)idtable_iterator_value(it);
                if (btc) {
                    bus_terminal_channel_release(btc);
                }
            }
            if (it) {
                idtable_iterator_release(it);
            }
            idtable_release(bt->send_channels);
            bt->send_channels = NULL;
        }
        // release recv channels
        if (bt->recv_channels) {
            it = idtable_iterator_init(bt->recv_channels, 0);
            while (it && idtable_iterator_loop(it) == 0) {
                btc = (struct bus_terminal_channel_t*)idtable_iterator_value(it);
                if (btc) {
                    bus_terminal_channel_release(btc);
                }
            }
            if (it) {
                idtable_iterator_release(it);
            }
            idtable_release(bt->recv_channels);
            bt->recv_channels = NULL;
        }
        FREE(bt);
    }
}

// check bus version and do update
// we should do dispatch in every tick
void bus_terminal_dispatch(bus_terminal_t* bt)
{
    uint32_t terminal_version, channel_version;
    if (!bt) return;

    // update terminals
    terminal_version = bt->bus->terminal_version;
    if (terminal_version != bt->local_terminal_version) {
        // nothing to do
        bt->local_terminal_version = terminal_version;
    }

    // udpate channels
    channel_version = bt->bus->channel_version;
    if (channel_version != bt->local_channel_version) {
        process_lock_lock(bt->lock);
        _bus_terminal_update_channels(bt);
        process_lock_unlock(bt->lock);
        bt->local_channel_version = channel_version;
    }
}

int32_t bus_terminal_send(bus_terminal_t* bt, const char* buf,
                          size_t buf_size, bus_addr_t to)
{
    struct bus_terminal_channel_t* btc;
    int32_t ret;

    if (!bt) return bus_err_fail;
    btc = (struct bus_terminal_channel_t*)idtable_get(bt->send_channels, to);
    if (!btc) return bus_err_peer_not_found;

    ret = rbuffer_write(bus_terminal_channel_rbuffer(btc), buf, buf_size);
    return ret == 0 ? bus_err_send_fail : bus_ok;
}

int32_t bus_terminal_send_by_type(bus_terminal_t* bt, const char* buf,
                                  size_t buf_size, int bus_type)
{
    struct bus_terminal_channel_t* btc;
    struct idtable_iterator_t* it;
    struct bus_channel_t* bc;
    int32_t ret;
    int32_t send = -1;

    if (!bt) return bus_err_fail;
    it = idtable_iterator_init(bt->send_channels, 0);
    if (!it) return bus_err_fail;

    while (idtable_iterator_loop(it) == 0) {
        btc = (struct bus_terminal_channel_t*)idtable_iterator_value(it);
        assert(btc);
        bc = bus_terminal_channel(btc);
        assert(bc);

        // if destinate bus type
        if (bus_addr_type(bc->to) == bus_type) {
            ret = rbuffer_write(bus_terminal_channel_rbuffer(btc), buf, buf_size);
            if (ret) {
                return bus_err_send_fail;
            } else {
                send = 0;
            }
        }
    }
    idtable_iterator_release(it);
    return send == 0 ? bus_ok : bus_err_peer_not_found;
}

int32_t bus_terminal_recv(bus_terminal_t* bt, char* buf,
                          size_t* buf_size, bus_addr_t from)
{
    struct bus_terminal_channel_t* btc;
    int32_t ret;

    if (!bt) return bus_err_fail;
    btc = (struct bus_terminal_channel_t*)idtable_get(bt->recv_channels, from);
    if (!btc) return bus_err_peer_not_found;

    ret = rbuffer_read(bus_terminal_channel_rbuffer(btc), buf, buf_size);
    return ret == 0 ? bus_err_empty : bus_ok;
}

int32_t bus_terminal_recv_all(bus_terminal_t* bt, char* buf,
                              size_t* buf_size, bus_addr_t* from)
{
    static int32_t index = 0;
    int32_t ret;
    struct idtable_iterator_t* it;
    struct bus_terminal_channel_t* btc;
    struct bus_channel_t* bc;

    if (!bt || !buf || !buf_size || !from) {
        return bus_err_fail;
    }
    it = idtable_iterator_init(bt->recv_channels, (index ++) % BUS_MAX_TERMINAL_COUNT);
    while (it && idtable_iterator_loop(it) == 0) {
        btc = (struct bus_terminal_channel_t*)idtable_iterator_value(it);
        assert(btc);
        bc = bus_terminal_channel(btc);
        assert(bc);
        ret = rbuffer_read(bus_terminal_channel_rbuffer(btc), buf, buf_size);
        if (ret == 0) {
            *from = bc->from;
            if (it) {
                idtable_iterator_release(it);
            }
            return 0;
        }
    }
    if (it) {
        idtable_iterator_release(it);
    }
    return bus_err_empty;
}

void bus_terminal_dump(bus_terminal_t* bt, char* debug, size_t debug_size)
{
    struct rbuffer_t* r;
    struct bus_terminal_channel_t* btc;
    struct bus_channel_t* bc;
    struct idtable_iterator_t* it;

    if (bt) {
        debug[0] = 0;

        // dump recv channels
        it = idtable_iterator_init(bt->recv_channels, 0);
        while (it && idtable_iterator_loop(it) == 0) {
            btc = idtable_iterator_value(it);
            assert(btc);
            bc = bus_terminal_channel(btc);
            r = bus_terminal_channel_rbuffer(btc);
            assert(bc && r);
            snprintf(debug, debug_size - strnlen(debug, debug_size),
                "%d->%d: size=%d, read bytes %u, write bytes %d\n", bc->from, bc->to,
                (int)bc->channel_size, rbuffer_read_bytes(r), rbuffer_write_bytes(r));
        }
        if (it) {
            idtable_iterator_release(it);
            it = NULL;
        }

        // dump send channels
        it = idtable_iterator_init(bt->send_channels, 0);
        while (it && idtable_iterator_loop(it) == 0) {
            btc = idtable_iterator_value(it);
            assert(btc);
            bc = bus_terminal_channel(btc);
            r = bus_terminal_channel_rbuffer(btc);
            assert(bc && r);
            snprintf(debug, debug_size - strnlen(debug, debug_size),
                "%d->%d: size=%d, read bytes %u, write bytes %d\n", bc->from, bc->to,
                (int)bc->channel_size, rbuffer_read_bytes(r), rbuffer_write_bytes(r));
        }
        if (it) {
            idtable_iterator_release(it);
            it = NULL;
        }
    }
}

uint32_t bus_terminal_send_bytes(bus_terminal_t* bt, bus_addr_t to)
{
    struct bus_terminal_channel_t* btc;
    if (!bt) return 0;
    btc = (struct bus_terminal_channel_t*)idtable_get(bt->send_channels, to);
    if (!btc) return 0;
    return rbuffer_write_bytes(bus_terminal_channel_rbuffer(btc));
}

uint32_t bus_terminal_recv_bytes(bus_terminal_t* bt, bus_addr_t from)
{
    struct bus_terminal_channel_t* btc;
    if (!bt) return 0;
    btc = (struct bus_terminal_channel_t*)idtable_get(bt->recv_channels, from);
    if (!btc) return 0;
    return rbuffer_read_bytes(bus_terminal_channel_rbuffer(btc));
}



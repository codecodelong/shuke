//
// Created by yangyu on 17-6-15.
//

#include <assert.h>

#include "shuke.h"

/* Macros for printing using RTE_LOG */
#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER1

/* Max size of a single packet */
#define MAX_PACKET_SZ           2048

/* Size of the data buffer in each mbuf */
#define MBUF_DATA_SZ (MAX_PACKET_SZ + RTE_PKTMBUF_HEADROOM)

/* Number of mbufs in mempool that is created */
#define NB_MBUF                 (8192 * 16)

/* How many objects (mbufs) to keep in per-lcore mempool cache */
#define MEMPOOL_CACHE_SZ        MAX_PKT_BURST

/* Total octets in ethernet header */
#define KNI_ENET_HEADER_SIZE    14

/* Total octets in the FCS */
#define KNI_ENET_FCS_SIZE       4

#define KNI_US_PER_SECOND       1000000
#define KNI_SECOND_PER_DAY      86400

#define KNI_MAX_KTHREAD 32

/* Options for configuring ethernet port */
static struct rte_eth_conf port_conf = {
        .rxmode = {
                .header_split = 0,      /* Header Split disabled */
                .hw_ip_checksum = 0,    /* IP checksum offload disabled */
                .hw_vlan_filter = 0,    /* VLAN filtering disabled */
                .jumbo_frame = 0,       /* Jumbo Frame Support disabled */
                .hw_strip_crc = 0,      /* CRC stripped by hardware */
        },
        .txmode = {
                .mq_mode = ETH_MQ_TX_NONE,
        },
};

/* Mempool for mbufs */
static struct rte_mempool * pktmbuf_pool = NULL;

/* Structure type for recording kni interface specific stats */
struct kni_interface_stats {
    /* number of pkts received from NIC, and sent to KNI */
    uint64_t rx_packets;

    /* number of pkts received from NIC, but failed to send to KNI */
    uint64_t rx_dropped;

    /* number of pkts received from KNI, and sent to NIC */
    uint64_t tx_packets;

    /* number of pkts received from KNI, but failed to send to NIC */
    uint64_t tx_dropped;
};

/* kni device statistics array */
static struct kni_interface_stats kni_stats[RTE_MAX_ETHPORTS];

static int kni_change_mtu(uint8_t port_id, unsigned new_mtu);
static int kni_config_network_interface(uint8_t port_id, uint8_t if_up);

static void
kni_burst_free_mbufs(struct rte_mbuf **pkts, unsigned num)
{
    unsigned i;

    if (pkts == NULL)
        return;

    for (i = 0; i < num; i++) {
        rte_pktmbuf_free(pkts[i]);
        pkts[i] = NULL;
    }
}

/**
 * Interface to dequeue mbufs from tx_q and burst tx
 */
static void
kni_egress(port_kni_conf_t *p)
{
    uint8_t port_id;
    unsigned nb_tx, num;
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];

    if (p == NULL)
        return;

    port_id = p->port_id;

    /* Burst rx from kni */
    num = rte_kni_rx_burst(p->kni, pkts_burst, MAX_PKT_BURST);
    if (unlikely(num > MAX_PKT_BURST)) {
        RTE_LOG(ERR, APP, "Error receiving from KNI\n");
        return;
    }
    /* Burst tx to eth */
    nb_tx = rte_eth_tx_burst(port_id, p->tx_queue_id, pkts_burst, (uint16_t)num);
    kni_stats[port_id].tx_packets += nb_tx;
    if (unlikely(nb_tx < num)) {
        /* Free mbufs not tx to NIC */
        kni_burst_free_mbufs(&pkts_burst[nb_tx], num - nb_tx);
        kni_stats[port_id].tx_dropped += num - nb_tx;
    }
}

static int
main_loop(__rte_unused void *arg)
{
    port_kni_conf_t *kconf = arg;
    const unsigned lcore_id = rte_lcore_id();
    assert(lcore_id == (unsigned) kconf->lcore_tx);

    RTE_LOG(INFO, APP, "Lcore %u is writing to port %d\n", kconf->lcore_tx, kconf->port_id);
    while (1) {
        if (sk.force_quit)
            break;
        kni_egress(kconf);
    }
    return 0;
}

/* Callback for request of changing MTU */
static int
kni_change_mtu(uint8_t port_id, unsigned new_mtu)
{
    int ret;
    struct rte_eth_conf conf;

    if (port_id >= rte_eth_dev_count()) {
        RTE_LOG(ERR, APP, "Invalid port id %d\n", port_id);
        return -EINVAL;
    }

    RTE_LOG(INFO, APP, "Change MTU of port %d to %u\n", port_id, new_mtu);

    /* Stop specific port */
    rte_eth_dev_stop(port_id);

    memcpy(&conf, &port_conf, sizeof(conf));
    /* Set new MTU */
    if (new_mtu > ETHER_MAX_LEN)
        conf.rxmode.jumbo_frame = 1;
    else
        conf.rxmode.jumbo_frame = 0;

    /* mtu + length of header + length of FCS = max pkt length */
    conf.rxmode.max_rx_pkt_len = new_mtu + KNI_ENET_HEADER_SIZE +
                                 KNI_ENET_FCS_SIZE;
    ret = rte_eth_dev_configure(port_id, 1, 1, &conf);
    if (ret < 0) {
        RTE_LOG(ERR, APP, "Fail to reconfigure port %d\n", port_id);
        return ret;
    }

    /* Restart specific port */
    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        RTE_LOG(ERR, APP, "Fail to restart port %d\n", port_id);
        return ret;
    }

    return 0;
}

/* Callback for request of configuring network interface up/down */
static int
kni_config_network_interface(uint8_t port_id, uint8_t if_up)
{
    int ret = 0;

    if (port_id >= rte_eth_dev_count() || port_id >= RTE_MAX_ETHPORTS) {
        RTE_LOG(ERR, APP, "Invalid port id %d\n", port_id);
        return -EINVAL;
    }

    RTE_LOG(INFO, APP, "Configure network interface of %d %s\n",
            port_id, if_up ? "up" : "down");

    return ret;
}

static int
kni_alloc(uint8_t port_id)
{
    port_kni_conf_t *kconf = sk.kni_conf[port_id];
    struct rte_kni *kni;
    struct rte_kni_conf conf;

    assert(port_id < RTE_MAX_ETHPORTS);

    /* Clear conf at first */
    memset(&conf, 0, sizeof(conf));
    strncpy(conf.name, kconf->name, RTE_KNI_NAMESIZE);
    if (kconf->lcore_k > 0) {
        conf.core_id = kconf->lcore_k;
        conf.force_bind = 1;
    }
    conf.group_id = (uint16_t)port_id;
    conf.mbuf_size = MAX_PACKET_SZ;
    /*
     * The first KNI device associated to a port
     * is the master, for multiple kernel thread
     * environment.
     */
    struct rte_kni_ops ops;
    struct rte_eth_dev_info dev_info;

    memset(&dev_info, 0, sizeof(dev_info));
    rte_eth_dev_info_get(port_id, &dev_info);
    conf.addr = dev_info.pci_dev->addr;
    conf.id = dev_info.pci_dev->id;

    memset(&ops, 0, sizeof(ops));
    ops.port_id = port_id;
    ops.change_mtu = kni_change_mtu;
    ops.config_network_if = kni_config_network_interface;

    kni = rte_kni_alloc(pktmbuf_pool, &conf, &ops);

    if (!kni)
        rte_exit(EXIT_FAILURE, "Fail to create kni for "
                "port: %d\n", port_id);
    sk.kni_conf[port_id]->kni = kni;

    return 0;
}

/* Initialize KNI subsystem */
void
init_kni(void)
{
    int portid = sk.port_ids[0];
    int lcore_id = sk.kni_conf[portid]->lcore_tx;
    int socket_id = rte_lcore_to_socket_id((unsigned) lcore_id);
    /* Create the mbuf pool */
    pktmbuf_pool = rte_pktmbuf_pool_create("kni_mbuf_pool", NB_MBUF,
                                           MEMPOOL_CACHE_SZ, 0, MBUF_DATA_SZ, socket_id);
    if (pktmbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Could not initialise mbuf pool\n");
    }

    for (int i = 0; i < sk.nr_ports; ++i) {
        int portid = sk.port_ids[i];
        kni_alloc(portid);
    }
}

int
free_kni()
{
    /* Release resources */
    for (int i = 0; i < sk.nr_ports; i++) {
        int portid = sk.port_ids[i];
        if (rte_kni_release(sk.kni_conf[portid]->kni))
            printf("Fail to release kni\n");
    }
#ifdef RTE_LIBRTE_XEN_DOM0
    rte_kni_close();
#endif

    // rte_eth_dev_stop(port_id);
    return 0;
}

port_kni_conf_t *getKniConf(unsigned lcore_id) {
    for (int i = 0; i < sk.nr_ports; ++i) {
        if (sk.kni_conf[i]->lcore_tx == (int)lcore_id) return sk.kni_conf[i];
    }
    return NULL;
}

/* Initialise ports/queues etc. and start main loop on each core */
int
start_kni_tx_threads()
{
    int ret;
    /* Launch per-lcore function on every lcore */
    for (int i=0; i < sk.nr_kni_tx_lcore_id; ++i) {
        unsigned lcore_id = sk.kni_tx_lcore_ids[i];
        port_kni_conf_t *kconf = getKniConf(lcore_id);
        assert(kconf);
        ret = rte_eal_remote_launch(main_loop, kconf, lcore_id);
        if (ret != 0) {
            rte_exit(EXIT_FAILURE, "Failed to start kni tx lcore %d, return %d", lcore_id, ret);
        }
    }
    return 0;
}
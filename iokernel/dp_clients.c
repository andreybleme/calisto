/*
 * dp_clients.c - functions for registering/unregistering dataplane clients
 */

#include <rte_ether.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_lcore.h>

#include <base/log.h>
#include <base/lrpc.h>

#include "defs.h"

#define MAC_TO_PROC_ENTRIES	128
#define HASH_TO_CORE_ENTRIES 500
#define RSS_HASH_LEN 52

static struct lrpc_chan_out lrpc_data_to_control;
static struct lrpc_chan_in lrpc_control_to_data;

/*
 * Add a new client.
 */
static void dp_clients_add_client(struct proc *p)
{
	int ret;
	struct rte_hash_parameters hash_params = { 0 };

	p->kill = false;
	dp.clients[dp.nr_clients++] = p;

	ret = rte_hash_add_key_data(dp.mac_to_proc, &p->mac.addr[0], p);
	if (ret < 0)
		log_err("dp_clients: failed to add MAC to hash table in add_client");

	/* initialize the hash table for mapping RSS hashes to CPU cores */
	hash_params.name = "hash_to_core_hash_table";
	hash_params.entries = HASH_TO_CORE_ENTRIES;
	hash_params.key_len = sizeof(uint32_t);
	hash_params.hash_func = rte_jhash;
	hash_params.hash_func_init_val = 0;
	dp.hash_to_core = rte_hash_create(&hash_params);
	if (dp.hash_to_core == NULL) {
		log_err("dp.hash_to_core: failed to create HASH to CORE hash table");
		return -1;
	}

#ifdef MLX
	p->mr = mlx_reg_mem(dp.port, p->region.base, p->region.len, &p->lkey);
	if (!p->mr)
		log_err("dp clients: failed to register memory with MLX nic");
#endif

	cores_init_proc(p);
}

void proc_release(struct ref *r)
{
	struct proc *p = container_of(r, struct proc, ref);
	if (!lrpc_send(&lrpc_data_to_control, CONTROL_PLANE_REMOVE_CLIENT,
			(unsigned long) p))
		log_err("dp_clients: failed to inform control of client removal");
}

/*
 * Remove a client. Notify control plane once removal is complete so that it
 * can delete its data structures.
 */
static void dp_clients_remove_client(struct proc *p)
{
	int i, ret;

	for (i = 0; i < dp.nr_clients; i++) {
		if (dp.clients[i] == p)
			break;
	}

	if (i == dp.nr_clients) {
		WARN();
		return;
	}

	dp.clients[i] = dp.clients[dp.nr_clients - 1];
	dp.nr_clients--;

	ret = rte_hash_del_key(dp.mac_to_proc, &p->mac.addr[0]);
	if (ret < 0)
		log_err("dp_clients: failed to remove MAC from hash table in remove "
				"client");
#ifdef MLX
	mlx_dereg_mem(p->mr);
#endif

	/* TODO: free queued packets/commands? */

	/* release cores assigned to this runtime */
	p->kill = true;
	cores_free_proc(p);
	proc_put(p);
}

/*
 * Process a batch of messages from the control plane.
 */
void dp_clients_rx_control_lrpcs()
{
	uint64_t cmd;
	unsigned long payload;
	uint16_t n_rx = 0;
	struct proc *p;

	while (lrpc_recv(&lrpc_control_to_data, &cmd, &payload)
			&& n_rx < IOKERNEL_CONTROL_BURST_SIZE) {
		p = (struct proc *) payload;

		switch (cmd)
		{
		case DATAPLANE_ADD_CLIENT:
			dp_clients_add_client(p);
			break;
		case DATAPLANE_REMOVE_CLIENT:
			dp_clients_remove_client(p);
			break;
		default:
			log_err("dp_clients: received unrecognized command %lu", cmd);
		}

		n_rx++;
	}
}

/*
 * Initialize channels for communicating with the I/O kernel control plane.
 */
int dp_clients_init(void)
{
	int ret;
	struct rte_hash_parameters hash_params = { 0 };

	ret = lrpc_init_in(&lrpc_control_to_data,
			lrpc_control_to_data_params.buffer, CONTROL_DATAPLANE_QUEUE_SIZE,
			lrpc_control_to_data_params.wb);
	if (ret < 0) {
		log_err("dp_clients: initializing LRPC from control plane failed");
		return -1;
	}

	ret = lrpc_init_out(&lrpc_data_to_control,
			lrpc_data_to_control_params.buffer, CONTROL_DATAPLANE_QUEUE_SIZE,
			lrpc_data_to_control_params.wb);
	if (ret < 0) {
		log_err("dp_clients: initializing LRPC to control plane failed");
		return -1;
	}

	dp.nr_clients = 0;

	/* initialize the hash table for mapping MACs to runtimes */
	hash_params.name = "mac_to_proc_hash_table";
	hash_params.entries = MAC_TO_PROC_ENTRIES;
	hash_params.key_len = ETHER_ADDR_LEN;
	hash_params.hash_func = rte_jhash;
	hash_params.hash_func_init_val = 0;
	hash_params.socket_id = rte_socket_id();
	dp.mac_to_proc = rte_hash_create(&hash_params);
	if (dp.mac_to_proc == NULL) {
		log_err("dp_clients: failed to create MAC to proc hash table");
		return -1;
	}

	return 0;
}

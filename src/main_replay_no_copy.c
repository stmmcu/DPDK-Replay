/*
*
* Copyright (c) 2015
*      Politecnico di Torino.  All rights reserved.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* For bug report and other information please write to:
* martino.trevisan@studenti.polito.it
*
*
*/

#include "main.h"

/* Constants of the system */
#define MEMPOOL_NAME "cluster_mem_pool"				// Name of the NICs' mem_pool, useless comment....
#define MEMPOOL_ELEM_SZ 2048  					// Power of two greater than 1500
#define MEMPOOL_CACHE_SZ 512  					// Max is 512


#define RX_QUEUE_SZ 256			// The size of rx queue. Max is 4096 and is the one you'll have best performances with. Use lower if you want to use Burst Bulk Alloc.
#define TX_QUEUE_SZ 4096			// Unused, you don't tx packets


/* Global vars */
char * file_name = NULL;
FILE * file;
uint64_t buffer_size = 1048576;
int do_shutdown = 0;
int times = 1;
double rate = 0;
int nb_sys_ports;
static struct rte_mempool * pktmbuf_pool;

uint64_t num_pkt_good_sent = 0;
uint64_t num_bytes_good_sent = 0;
uint64_t old_num_pkt_good_sent = 0;
uint64_t old_num_bytes_good_sent = 0;

struct timeval start_time;
struct timeval last_time;

/* Main function */
int main(int argc, char **argv)
{
	int ret;
	int i;

	/* Create handler for SIGINT for CTRL + C closing and SIGALRM to print stats*/
	signal(SIGINT, sig_handler);
	signal(SIGALRM, alarm_routine);

	/* Initialize DPDK enviroment with args, then shift argc and argv to get application parameters */
	ret = rte_eal_init(argc, argv);
	if (ret < 0) FATAL_ERROR("Cannot init EAL\n");
	argc -= ret;
	argv += ret;

	/* Check if this application can use 1 core*/
	ret = rte_lcore_count ();
	if (ret != 1) FATAL_ERROR("This application needs exactly 1 cores.");

	/* Parse arguments */
	parse_args(argc, argv);
	if (ret < 0) FATAL_ERROR("Wrong arguments\n");

	/* Probe PCI bus for ethernet devices, mandatory only in DPDK < 1.8.0 */
	#if RTE_VER_MAJOR == 1 && RTE_VER_MINOR < 8
		ret = rte_eal_pci_probe();
		if (ret < 0) FATAL_ERROR("Cannot probe PCI\n");
	#endif

	/* Get number of ethernet devices */
	nb_sys_ports = rte_eth_dev_count();
	if (nb_sys_ports <= 0) FATAL_ERROR("Cannot find ETH devices\n");
	
	/* Create a mempool with per-core cache, initializing every element for be used as mbuf, and allocating on the current NUMA node */
	pktmbuf_pool = rte_mempool_create(MEMPOOL_NAME, buffer_size-1, MEMPOOL_ELEM_SZ, MEMPOOL_CACHE_SZ, sizeof(struct rte_pktmbuf_pool_private), rte_pktmbuf_pool_init, NULL, rte_pktmbuf_init, NULL,rte_socket_id(), 0);
	if (pktmbuf_pool == NULL) FATAL_ERROR("Cannot create cluster_mem_pool. Errno: %d [ENOMEM: %d, ENOSPC: %d, E_RTE_NO_TAILQ: %d, E_RTE_NO_CONFIG: %d, E_RTE_SECONDARY: %d, EINVAL: %d, EEXIST: %d]\n", rte_errno, ENOMEM, ENOSPC, E_RTE_NO_TAILQ, E_RTE_NO_CONFIG, E_RTE_SECONDARY, EINVAL, EEXIST  );
	
	/* Operations needed for each ethernet device */			
	for(i=0; i < nb_sys_ports; i++)
		init_port(i);

	/* ... and then loop in consumer */
	main_loop_producer ( NULL );	

	return 0;
}



/* Loop function, batch timing implemented */
static int main_loop_producer(__attribute__((unused)) void * arg){
	struct rte_mbuf * m, * m_copy;
	struct timeval now;
	//struct ipv4_hdr * ip_h;
	double mult_start = 0, mult = 0, real_rate, deltaMillisec;
	struct pcaprec_hdr_t hdr;
	int i, ix, ret;
	uint64_t tick_start;

	/* Open the trace */
	printf("Opening file: %s\n", file_name);
	printf("Replay on %d interface(s)\n", nb_sys_ports);
	file = fopen(file_name, "r");
	if (file == NULL){	
		printf("Unable to open file: %s\n", file_name);
		exit(1);			
	}
	/* Prepare file pointer skiping pcap hdr, and setting large buffer */	
	fseek(file, sizeof(struct pcap_hdr_t), SEEK_SET);
	ret = setvbuf(file, NULL, _IOFBF, 33554432);
	if (ret != 0) FATAL_ERROR("Cannot set the size of the file pointer to the trace...\n");

	/* Prepare variables to rate setting if needed */
	if(rate != 0){
		mult_start = (double )rte_get_tsc_hz  () / 1000000000L; 
		mult = mult_start;
		ix = 0;
	}
	
	/* Init start time */
	ret = gettimeofday(&start_time, NULL);
	if (ret != 0) FATAL_ERROR("Error: gettimeofday failed. Quitting...\n");
	last_time = start_time;
	tick_start =   rte_get_tsc_cycles();

	/* Start stats */
   	alarm(1);

	for (i=0;i<nb_sys_ports; i++)
		rte_eth_stats_reset ( i );

	/* Infinite loop */
	for (;;) {

		/* If the system is quitting, break the cycle */
		if (unlikely(do_shutdown))
			break;

		/* Read packet from trace */
		ret = fread((void*)&hdr, sizeof (hdr), 1, file);
		if(unlikely(ret <= 0)) break;

		/* Alloc the buffer */
		m =  rte_pktmbuf_alloc 	(pktmbuf_pool);

		/* Read data from trace */
		ret = fread((void*)((char*) m->buf_addr + m->data_off ), hdr.incl_len, 1 , file );
		if(unlikely(ret <= 0)) break;

		/* Compile the buffer length */
		m->data_len = m->pkt_len = hdr.incl_len;
	
		/* For each packet to send */
		for (i = 0; likely( i < nb_sys_ports*times ) ; i++) {

			/* On the first port, send original packet buffer */
			if (unlikely(i==0)){
				while ( rte_eth_tx_burst (i, 0, &m , 1) != 1)
				if (unlikely(do_shutdown)) break;
			}
			/* On the other ports, send indirect buffers pointing to the original one */
			else {
				m_copy = rte_pktmbuf_clone( m,pktmbuf_pool) ;
				/* Loop untill it is not sent */
				while ( rte_eth_tx_burst (i/times, 0, &m_copy , 1) != 1)
					if (unlikely(do_shutdown)) break;
			}
		}


		/* Rate set */
		if(rate > 0) {
			/* Adjust the rate every 100 packets sent */
			if (ix++%100 ==0){
				/* Calculate the actual rate */
				gettimeofday(&now, NULL);
				deltaMillisec = (double)(now.tv_sec - start_time.tv_sec ) * 1000 + (double)(now.tv_usec - start_time.tv_usec ) / 1000 ;
				real_rate = (double)(num_bytes_good_sent * 1000)/deltaMillisec * 8/(1000*1000*1000);
				mult = mult + (real_rate - rate); // CONTROL LAW;

				/* Avoid negative numbers. Avoid problems when the NICs are stuck for a while */
				if (mult < 0) mult = 0;
			}
			/* Wait to adjust the rate*/
			while(( rte_get_tsc_cycles() - tick_start) < (num_bytes_good_sent * mult / rate )) 
				if (unlikely(do_shutdown)) break;
		}

		/* Update stats */
		num_pkt_good_sent += times;
		num_bytes_good_sent += (hdr.incl_len + 24) * times; /* 8 Preamble + 4 CRC + 12 IFG*/

	}

	sig_handler(SIGINT);
	return 0;
}

void print_stats (void){
	int ret;
	struct timeval now_time;
	double delta_ms;
	double tot_ms;
	double gbps_inst, gbps_tot, mpps_inst, mpps_tot;

	/* Get actual time */
	ret = gettimeofday(&now_time, NULL);
	if (ret != 0) FATAL_ERROR("Error: gettimeofday failed. Quitting...\n");

	/* Compute stats */
	delta_ms =  (now_time.tv_sec - last_time.tv_sec ) * 1000 + (now_time.tv_usec - last_time.tv_usec ) / 1000 ;
	tot_ms = (now_time.tv_sec - start_time.tv_sec ) * 1000 + (now_time.tv_usec - start_time.tv_usec ) / 1000 ;
	gbps_inst = (double)(num_bytes_good_sent - old_num_bytes_good_sent)/delta_ms/1000000*8;
	gbps_tot = (double)(num_bytes_good_sent)/tot_ms/1000000*8;
	mpps_inst = (double)(num_pkt_good_sent - old_num_pkt_good_sent)/delta_ms/1000;
	mpps_tot = (double)(num_pkt_good_sent)/tot_ms/1000;

	printf("Rate: %8.3fGbps  %8.3fMpps [Average rate: %8.3fGbps  %8.3fMpps]\n", gbps_inst, mpps_inst, gbps_tot, mpps_tot);

	/* Update counters */
	old_num_bytes_good_sent = num_bytes_good_sent;
	old_num_pkt_good_sent = num_pkt_good_sent;
	last_time = now_time;

}

void alarm_routine (__attribute__((unused)) int unused){

	/* If the program is quitting don't print anymore */
	if(do_shutdown) return;

	/* Print per port stats */
	print_stats();

	/* Schedule an other print */
	alarm(1);
	signal(SIGALRM, alarm_routine);

}

/* Signal handling function */
static void sig_handler(int signo)
{
	uint64_t diff;
	int ret;
	struct timeval t_end;

	/* Catch just SIGINT */
	if (signo == SIGINT){

		/* Signal the shutdown */
		do_shutdown=1;

		/* Print the per stats  */
		printf("\n\nQUITTING...\n");
		ret = gettimeofday(&t_end, NULL);
		if (ret != 0) FATAL_ERROR("Error: gettimeofday failed. Quitting...\n");		
		diff = t_end.tv_sec - start_time.tv_sec;
		printf("The replay lasted %ld seconds. Sent %ld packets on every interface\n", diff, num_pkt_good_sent);
		print_stats();

		/* Close the pcap file */
		exit(0);	
	}
}

/* Init each port with the configuration contained in the structs. Every interface has nb_sys_cores queues */
static void init_port(int i) {

		int ret;
		uint8_t rss_key [40];
		struct rte_eth_link link;
		struct rte_eth_dev_info dev_info;
		struct rte_eth_rss_conf rss_conf;
		struct rte_eth_fdir fdir_conf;

		/* Retreiving and printing device infos */
		rte_eth_dev_info_get(i, &dev_info);
		printf("Name:%s\n\tDriver name: %s\n\tMax rx queues: %d\n\tMax tx queues: %d\n", dev_info.pci_dev->driver->name,dev_info.driver_name, dev_info.max_rx_queues, dev_info.max_tx_queues);
		printf("\tPCI Adress: %04d:%02d:%02x:%01d\n", dev_info.pci_dev->addr.domain, dev_info.pci_dev->addr.bus, dev_info.pci_dev->addr.devid, dev_info.pci_dev->addr.function);

		/* Configure device with '1' rx queues and 1 tx queue */
		ret = rte_eth_dev_configure(i, 1, 1, &port_conf);
		if (ret < 0) rte_panic("Error configuring the port\n");

		/* For each RX queue in each NIC */
		/* Configure rx queue j of current device on current NUMA socket. It takes elements from the mempool */
		ret = rte_eth_rx_queue_setup(i, 0, RX_QUEUE_SZ, rte_socket_id(), &rx_conf, pktmbuf_pool);
		if (ret < 0) FATAL_ERROR("Error configuring receiving queue\n");
		/* Configure mapping [queue] -> [element in stats array] */
		ret = rte_eth_dev_set_rx_queue_stats_mapping 	(i, 0, 0);
		if (ret < 0) FATAL_ERROR("Error configuring receiving queue stats\n");


		/* Configure tx queue of current device on current NUMA socket. Mandatory configuration even if you want only rx packet */
		ret = rte_eth_tx_queue_setup(i, 0, TX_QUEUE_SZ, rte_socket_id(), &tx_conf);
		if (ret < 0) FATAL_ERROR("Error configuring transmitting queue. Errno: %d (%d bad arg, %d no mem)\n", -ret, EINVAL ,ENOMEM);

		/* Start device */		
		ret = rte_eth_dev_start(i);
		if (ret < 0) FATAL_ERROR("Cannot start port\n");

		/* Enable receipt in promiscuous mode for an Ethernet device */
		rte_eth_promiscuous_enable(i);

		/* Print link status */
		rte_eth_link_get_nowait(i, &link);
		if (link.link_status) 	printf("\tPort %d Link Up - speed %u Mbps - %s\n", (uint8_t)i, (unsigned)link.link_speed,(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?("full-duplex") : ("half-duplex\n"));
		else			printf("\tPort %d Link Down\n",(uint8_t)i);

		/* Print RSS support, not reliable because a NIC could support rss configuration just in rte_eth_dev_configure whithout supporting rte_eth_dev_rss_hash_conf_get*/
		rss_conf.rss_key = rss_key;
		ret = rte_eth_dev_rss_hash_conf_get (i,&rss_conf);
		if (ret == 0) printf("\tDevice supports RSS\n"); else printf("\tDevice DOES NOT support RSS\n");
		
		/* Print Flow director support */
		ret = rte_eth_dev_fdir_get_infos (i, &fdir_conf);
		if (ret == 0) printf("\tDevice supports Flow Director\n"); else printf("\tDevice DOES NOT support Flow Director\n"); 

	
}

static int parse_args(int argc, char **argv)
{
	int option;
	

	/* Retrive arguments */
	while ((option = getopt(argc, argv,"f:t:r:B:")) != -1) {
        	switch (option) {
             		case 'f' : file_name = strdup(optarg); /* File name, mandatory */
                 		break;
			case 't': times = atoi (optarg); /* Times to send packet on each port. Default is 1 */
				break;
			case 'B': buffer_size = atoi (optarg); /* Buffer size in packets. Must be a power of two . Default is 1048576 */
				break;
			case 'r': rate = atof (optarg); /* Rate in Gbps */
				break;
             		default: return -1; 
		}
   	}

	/* Returning bad value in case of wrong arguments */
	if(file_name == NULL || isPowerOfTwo (buffer_size)!=1 )
		return -1;

	return 0;

}

int isPowerOfTwo (unsigned int x)
{
  return ((x != 0) && !(x & (x - 1)));
}



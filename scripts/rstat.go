package main

import (
	"net"
	"os"
	"time"
	"strconv"
	"fmt"
	"strings"
)

func prettyPrint(m, lastm map[string]uint64, interval int) {
	dm := make(map[string]float64)
	for i, v := range m {
		dm[i] = float64(v - lastm[i]) / float64(interval)
	}
	fmt.Printf("net: RX %.1f pkts, %.1f bytes | TX %.1f pkts, %.1f bytes | %.1f drops | %.2f%% rx out of order (%.2f%% reorder time)\n",
		   dm["rx_packets"], dm["rx_bytes"],
		   dm["tx_packets"], dm["tx_bytes"], dm["drops"],
		   dm["rx_tcp_out_of_order"] / (dm["rx_tcp_in_order"] + dm["rx_tcp_out_of_order"]) * 100,
		   dm["rx_tcp_text_cycles"] / (dm["sched_cycles"] + dm["program_cycles"]) * 100)
	fmt.Printf("sched: %.1f rescheds (%.1f%% sched time, %.1f%% local)," +
		   " %.1f softirqs (%.1f%% stolen), %.1f %%CPU, %.1f parks" +
		   " (%.1f%% migrated), %.1f preempts (%.1f stolen)\n",
		   dm["reschedules"],
		   dm["sched_cycles"] / (dm["sched_cycles"] + dm["program_cycles"]) * 100,
		   (1 - dm["threads_stolen"] / dm["reschedules"]) * 100,
		   dm["softirqs_local"] + dm["softirqs_stolen"],
		   (dm["softirqs_stolen"] / (dm["softirqs_local"] +
		    dm["softirqs_stolen"])) * 100,
		   (dm["sched_cycles"] + dm["program_cycles"]) * 100 /
		    (float64(m["cycles_per_us"]) * 1000000), dm["parks"],
		   dm["core_migrations"] * 100 / dm["parks"],
		   dm["preemptions"], dm["preemptions_stolen"])
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintf(os.Stderr, "usage:%s [host] [interval]", os.Args[0])
		os.Exit(1)
	}

	host := os.Args[1]
	uaddr, err := net.ResolveUDPAddr("udp4", host + ":40")
	if err != nil {
		os.Exit(1)
	}

	c, err := net.DialUDP("udp", nil, uaddr)
	if err != nil {
		os.Exit(1)
	}

	interval, err := strconv.Atoi(os.Args[2])
	if err != nil {
		os.Exit(1)
	}

	var buf [1500]byte
	lastm := make(map[string]uint64)

	for {
		_, err = c.Write([]byte("stat"))
		if err != nil {
			os.Exit(1)
		}

		c.SetReadDeadline(time.Now().Add(time.Duration(4) * time.Millisecond))
		n, err := c.Read(buf[0:])
		if err != nil {
			if nerr, ok := err.(net.Error); ok && nerr.Timeout() {
				lastm = make(map[string]uint64)
				continue
			}
			os.Exit(1)
		}
		strs := strings.Split(string(buf[0:n-1]), ",")
		m := make(map[string]uint64)

		for _, v := range strs {

			fields := strings.Split(v, ":")
			if len(fields) != 2 {
				fmt.Println("can't parse", v)
				os.Exit(1)
			}

			value, err := strconv.ParseUint(fields[1], 10, 64)
			if err != nil {
				fmt.Println("can't parse uint64", fields[1])
				os.Exit(1)
			}

			m[fields[0]] = value
		}

		if len(lastm) > 0 {
			prettyPrint(m, lastm, interval)
		}
		lastm = m

		time.Sleep(time.Duration(interval) * time.Second)
	}
}

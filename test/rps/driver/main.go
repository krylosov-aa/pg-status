// A bounded-memory measurement wrapper around Vegeta's constant-rate attacker.
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	hdr "github.com/HdrHistogram/hdrhistogram-go"
	vegeta "github.com/tsenart/vegeta/v12/lib"
)

type entry struct {
	path, expect string
}

type histogram struct {
	*hdr.Histogram
}

func newHistogram() histogram {
	return histogram{hdr.New(1, 3600000000, 3)}
}

func (h histogram) add(d time.Duration) {
	v := max(int64(1), d.Microseconds())
	if err := h.RecordValue(v); err != nil {
		panic(err)
	}
}

func (h histogram) summary() map[string]float64 {
	r := map[string]float64{
		"mean": h.Mean() / 1000,
		"max":  float64(h.Max()) / 1000,
	}
	for _, p := range []float64{50, 90, 95, 99, 99.9} {
		r[fmt.Sprint(p)] = float64(h.ValueAtQuantile(p)) / 1000
	}
	return r
}

func scheduledLatency(
	start time.Time, r *vegeta.Result, rate int,
) (time.Duration, time.Duration) {
	// ConstantPacer's first request is due after one integer-nanosecond interval.
	due := start.Add(time.Duration(r.Seq+1) * (time.Second / time.Duration(rate)))
	delay := max(time.Duration(0), r.Timestamp.Sub(due))
	return r.Latency + delay, delay
}

type routeStats struct {
	Requests int            `json:"requests"`
	Invalid  int            `json:"invalid"`
	Bodies   map[string]int `json:"selected_hosts,omitempty"`
}

type proc struct {
	CPU                    float64
	RSS, HWM, Threads, FDs int64
}

func readProc(pid int, hz float64) proc {
	b, err := os.ReadFile(fmt.Sprintf("/proc/%d/stat", pid))
	if err != nil {
		panic(err)
	}
	f := strings.Fields(string(b)[strings.LastIndex(string(b), ")")+2:])
	u, _ := strconv.ParseFloat(f[11], 64)
	s, _ := strconv.ParseFloat(f[12], 64)
	r := proc{CPU: (u + s) / hz}
	b, err = os.ReadFile(fmt.Sprintf("/proc/%d/status", pid))
	if err != nil {
		panic(err)
	}
	for _, line := range strings.Split(string(b), "\n") {
		v := strings.Fields(line)
		if len(v) < 2 {
			continue
		}
		n, _ := strconv.ParseInt(v[1], 10, 64)
		switch v[0] {
		case "VmRSS:":
			r.RSS = n
		case "VmHWM:":
			r.HWM = n
		case "Threads:":
			r.Threads = n
		}
	}
	fds, _ := os.ReadDir(fmt.Sprintf("/proc/%d/fd", pid))
	r.FDs = int64(len(fds))
	return r
}

func systemCPU() []float64 {
	b, err := os.ReadFile("/proc/stat")
	if err != nil {
		panic(err)
	}
	f := strings.Fields(strings.SplitN(string(b), "\n", 2)[0])
	v := make([]float64, 8)
	for i := range v {
		v[i], _ = strconv.ParseFloat(f[i+1], 64)
	}
	return v
}

type sample struct {
	Seconds       float64 `json:"seconds"`
	ServerCPU     float64 `json:"server_cpu_pct"`
	GeneratorCPU  float64 `json:"generator_cpu_pct"`
	ServerRSS     int64   `json:"server_rss_kib"`
	ServerHWM     int64   `json:"server_hwm_kib"`
	ServerThreads int64   `json:"server_threads"`
	ServerFDs     int64   `json:"server_fds"`
	GeneratorRSS  int64   `json:"generator_rss_kib"`
	HostBusy      float64 `json:"host_busy_pct"`
	HostSteal     float64 `json:"host_steal_pct"`
}

func monitor(pid int, hz float64, stop <-chan struct{}, done chan<- []sample) {
	start := time.Now()
	last := start
	ps := readProc(pid, hz)
	pg := readProc(os.Getpid(), hz)
	sys := systemCPU()
	var samples []sample
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		ending := false
		select {
		case <-ticker.C:
		case <-stop:
			ending = true
		}
		now := time.Now()
		dt := now.Sub(last).Seconds()
		s := readProc(pid, hz)
		g := readProc(os.Getpid(), hz)
		c := systemCPU()
		total := 0.0
		for i := range c {
			total += c[i] - sys[i]
		}
		if dt > 0 && total > 0 {
			samples = append(samples, sample{
				now.Sub(start).Seconds(),
				(s.CPU - ps.CPU) / dt * 100,
				(g.CPU - pg.CPU) / dt * 100,
				s.RSS,
				s.HWM,
				s.Threads,
				s.FDs,
				g.RSS,
				(total - (c[3] - sys[3]) - (c[4] - sys[4])) / total * 100,
				(c[7] - sys[7]) / total * 100,
			})
		}
		ps = s
		pg = g
		sys = c
		last = now
		if ending {
			done <- samples
			return
		}
	}
}

func validBody(
	body []byte, expect, primary string, replicas map[string]bool, jsonMode bool,
) (bool, string) {
	selected := string(body)
	if expect == "http" {
		return true, ""
	}
	if expect == "hosts" {
		var hosts []struct {
			Host   string
			Alive  bool
			Master bool
		}
		if json.Unmarshal(body, &hosts) != nil || len(hosts) != len(replicas)+1 {
			return false, ""
		}
		seen := map[string]bool{}
		for _, h := range hosts {
			if seen[h.Host] || !h.Alive ||
				(h.Host != primary && !replicas[h.Host]) ||
				h.Master != (h.Host == primary) {
				return false, ""
			}
			seen[h.Host] = true
		}
		return true, ""
	}
	if jsonMode {
		var host struct{ Host string }
		if json.Unmarshal(body, &host) != nil {
			return false, ""
		}
		selected = host.Host
	}
	switch expect {
	case "primary":
		return selected == primary, selected
	case "replica":
		return replicas[selected], selected
	case "route":
		return selected == primary || replicas[selected], selected
	}
	return false, selected
}

func main() {
	base := flag.String("url", "http://127.0.0.1:8000", "URL")
	profile := flag.String(
		"profile", "../profile.txt",
		"weighted profile; optional expectation: primary, replica, route, hosts, http",
	)
	rate := flag.Int("rate", 5000, "requests/sec")
	duration := flag.Duration("duration", 5*time.Minute, "measurement duration")
	warmup := flag.Duration("warmup", 30*time.Second, "warmup duration")
	connections := flag.Int("connections", 128, "connection limit")
	workers := flag.Int("workers", 512, "fixed Vegeta worker limit")
	timeout := flag.Duration("timeout", 2*time.Second, "request timeout")
	keepalive := flag.Bool("keepalive", true, "reuse TCP connections")
	jsonMode := flag.Bool("json", false, "Accept: application/json")
	primary := flag.String("primary", "127.0.0.1", "expected primary hostname")
	replicaList := flag.String(
		"replicas", "127.0.0.2,127.0.0.3", "expected replica hostnames",
	)
	lsn := flag.String("lsn", "", "replace {lsn} with this committed LSN")
	lsnFile := flag.String(
		"lsn-file", "", "refresh {lsn} from an atomically replaced file every 10ms",
	)
	pid := flag.Int("pid", 0, "pg-status PID, required for resource sampling")
	hz := flag.Float64("clock-ticks", 100, "getconf CLK_TCK")
	p99 := flag.Float64("max-p99-ms", 5, "scheduled p99 SLO")
	ratio := flag.Float64(
		"min-throughput-ratio", 0.999, "minimum validated throughput / offered rate",
	)
	out := flag.String("out", "report.json", "report path")
	flag.Parse()
	if *rate <= 0 || *duration <= 0 || *warmup < 0 || *connections <= 0 ||
		*workers <= 0 || *pid <= 0 || *hz <= 0 || *p99 < 0 ||
		*ratio <= 0 || *ratio > 1 {
		panic("invalid benchmark configuration")
	}
	file, err := os.Open(*profile)
	if err != nil {
		panic(err)
	}
	defer file.Close()
	var entries []entry
	rules := map[string]string{}
	scan := bufio.NewScanner(file)
	for scan.Scan() {
		line := strings.TrimSpace(scan.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		f := strings.Fields(line)
		if len(f) < 2 || len(f) > 3 {
			panic("profile: expected weight path [expectation]")
		}
		weight, e := strconv.Atoi(f[0])
		if e != nil || weight <= 0 || weight > 10000 || !strings.HasPrefix(f[1], "/") {
			panic("invalid profile row")
		}
		expect := "http"
		if len(f) == 3 {
			expect = f[2]
		}
		if !strings.Contains("|primary|replica|route|hosts|http|", "|"+expect+"|") {
			panic("unknown expectation")
		}
		key := strings.SplitN(f[1], "?", 2)[0]
		if old, ok := rules[key]; ok && old != expect {
			panic("conflicting expectations for path")
		}
		rules[key] = expect
		for i := 0; i < weight; i++ {
			entries = append(entries, entry{f[1], expect})
		}
	}
	if scan.Err() != nil || len(entries) == 0 {
		panic("empty or unreadable profile")
	}
	replicas := map[string]bool{}
	for _, s := range strings.Split(*replicaList, ",") {
		if s != "" {
			replicas[s] = true
		}
	}
	var currentLSN atomic.Value
	currentLSN.Store(*lsn)
	if *lsnFile != "" {
		read := func() {
			b, e := os.ReadFile(*lsnFile)
			if e != nil {
				panic(e)
			}
			currentLSN.Store(strings.TrimSpace(string(b)))
		}
		read()
		go func() {
			for {
				time.Sleep(10 * time.Millisecond)
				read()
			}
		}()
	}
	for _, e := range entries {
		if strings.Contains(e.path, "{lsn}") && currentLSN.Load().(string) == "" {
			panic("LSN required")
		}
	}
	newAttack := func() (*vegeta.Attacker, vegeta.Targeter) {
		a := vegeta.NewAttacker(
			vegeta.Connections(*connections),
			vegeta.MaxConnections(*connections),
			vegeta.Workers(uint64(*workers)),
			vegeta.MaxWorkers(uint64(*workers)),
			vegeta.Timeout(*timeout),
			vegeta.KeepAlive(*keepalive),
			vegeta.MaxBody(65536),
			vegeta.Redirects(-1),
		)
		var mu sync.Mutex
		index := 0
		tr := func(t *vegeta.Target) error {
			mu.Lock()
			e := entries[index%len(entries)]
			index++
			mu.Unlock()
			path := strings.ReplaceAll(e.path, "{lsn}", currentLSN.Load().(string))
			t.Method = "GET"
			t.URL = strings.TrimRight(*base, "/") + path
			t.Header = make(http.Header)
			if *jsonMode {
				t.Header.Set("Accept", "application/json")
			}
			return nil
		}
		return a, tr
	}
	if *warmup > 0 {
		fmt.Fprintln(os.Stderr, "warmup", *warmup)
		a, tr := newAttack()
		for range a.Attack(
			tr, vegeta.Rate{Freq: *rate, Per: time.Second}, *warmup, "warmup",
		) {
		}
	}
	lat := newHistogram()
	scheduled := newHistogram()
	delays := newHistogram()
	routes := map[string]*routeStats{}
	codes := map[uint16]int{}
	errors := map[string]int{}
	var metrics vegeta.Metrics
	invalid := 0
	good := 0
	a, tr := newAttack()
	stop := make(chan struct{})
	done := make(chan []sample)
	go monitor(*pid, *hz, stop, done)
	// Includes the interval before Attack captures its start time.
	start := time.Now()
	fmt.Fprintln(
		os.Stderr, "measurement", start.UTC().Format(time.RFC3339Nano), *duration,
	)
	for r := range a.Attack(
		tr, vegeta.Rate{Freq: *rate, Per: time.Second}, *duration, "",
	) {
		metrics.Add(r)
		lat.add(r.Latency)
		corrected, delay := scheduledLatency(start, r, *rate)
		scheduled.add(corrected)
		delays.add(delay)
		key := strings.SplitN(
			strings.TrimPrefix(r.URL, strings.TrimRight(*base, "/")), "?", 2,
		)[0]
		stats := routes[key]
		if stats == nil {
			stats = &routeStats{Bodies: map[string]int{}}
			routes[key] = stats
		}
		stats.Requests++
		ok, selected := validBody(r.Body, rules[key], *primary, replicas, *jsonMode)
		if selected != "" {
			stats.Bodies[selected]++
		}
		if r.Error != "" || r.Code != 200 || !ok {
			invalid++
			stats.Invalid++
		} else {
			good++
		}
		codes[r.Code]++
		if r.Error != "" && len(errors) < 100 {
			errors[r.Error]++
		}
	}
	elapsed := time.Since(start)
	close(stop)
	samples := <-done
	metrics.Close()
	throughput := float64(good) / max(elapsed.Seconds(), duration.Seconds())
	pass := invalid == 0 && throughput >= float64(*rate)*(*ratio) &&
		scheduled.summary()["99"] <= *p99
	report := map[string]any{
		"started":              start.UTC(),
		"elapsed_seconds":      elapsed.Seconds(),
		"offered_rps":          *rate,
		"duration_seconds":     duration.Seconds(),
		"warmup_seconds":       warmup.Seconds(),
		"connections":          *connections,
		"workers":              *workers,
		"keepalive":            *keepalive,
		"json":                 *jsonMode,
		"profile":              *profile,
		"lsn":                  *lsn,
		"lsn_file":             *lsnFile,
		"requests":             metrics.Requests,
		"invalid":              invalid,
		"validated_rps":        throughput,
		"pass":                 pass,
		"max_p99_ms":           *p99,
		"min_throughput_ratio": *ratio,
		"http_latency_ms":      lat.summary(),
		"scheduled_latency_ms": scheduled.summary(),
		"dispatch_delay_ms":    delays.summary(),
		"status_codes":         codes,
		"errors":               errors,
		"routes":               routes,
		"resources":            samples,
		"vegeta":               metrics,
		"arguments":            os.Args,
	}
	b, err := json.MarshalIndent(report, "", "  ")
	if err != nil {
		panic(err)
	}
	if err = os.WriteFile(*out, append(b, '\n'), 0644); err != nil {
		panic(err)
	}
	fmt.Fprintf(
		os.Stderr,
		"pass=%t offered=%d validated=%.1f scheduled_p99=%.3fms http_p99=%.3fms invalid=%d\n",
		pass, *rate, throughput, scheduled.summary()["99"], lat.summary()["99"], invalid,
	)
	if !pass {
		os.Exit(1)
	}
}

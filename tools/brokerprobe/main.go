// brokerprobe — measures, from a third consumer, whether the payloads the
// node considers published actually reach the broker.
//
// The question it answers: of the cycles the node logs as LOG_PUBLISH_OK
// that don't show up on any subscriber, did the PUBLISH make it into the
// broker and get lost inside, or did it never arrive?
//
// A third subscriber isn't enough for that: if the broker drops the message
// on ingress, ALL subscribers lose it the same way. What does discriminate
// are the $SYS counters, which Mosquitto increments in handle__publish,
// before routing:
//
//   $SYS/broker/publish/messages/received  → PUBLISH messages ingested
//   $SYS/broker/publish/bytes/received     → bytes, to confirm the
//                                            ingested message was telemetry
//                                            (~480 B) and not a short one
//
// And along the way, $SYS/broker/clients/connected gives the lifetime of
// the node's session inside the broker, which is the premise behind the
// takeover hypothesis: if the session stays alive between cycles (~90 s
// with keepalive 60), every reconnection forces a takeover via duplicate
// client ID.
//
// Mosquitto publishes each $SYS value only when it changes, so the message
// sequence IS the sequence of transitions, with its timestamp.
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

type event struct {
	T     string `json:"t"`
	Topic string `json:"topic"`
	Value string `json:"value,omitempty"`
	// telemetry only
	BootCount *int64 `json:"boot_count,omitempty"`
	RSSI      *int64 `json:"rssi,omitempty"`
	Firmware  string `json:"firmware,omitempty"`
	Bytes     int    `json:"bytes,omitempty"`
	// The full payload. The first version only extracted the four fields
	// above, and when it became necessary to cross-check losses against
	// `system_v` —the hypothesis that power sags during transmission
	// spikes— the data had already passed through here and hadn't been
	// saved. Saving everything costs nothing and avoids having to repeat a
	// 35-minute window.
	Doc map[string]any `json:"doc,omitempty"`
}

type probe struct {
	mu  sync.Mutex
	enc *json.Encoder
	w   *bufio.Writer

	start time.Time

	// telemetry delivered
	telemetry  int
	bootCounts []int64
	firmware   string

	// $SYS counters (first value seen and last)
	sysFirst map[string]uint64
	sysLast  map[string]uint64

	// clients/connected transitions
	connSamples []connSample
}

type connSample struct {
	at time.Time
	n  uint64
}

func main() {
	broker := flag.String("broker", "tcp://192.168.18.250:1883", "broker URL")
	user := flag.String("user", "weather_station_iot", "MQTT user")
	pass := flag.String("pass", "aXdC7nE2gLEe", "MQTT password")
	topic := flag.String("topic", "station/01/telemetry", "telemetry topic")
	dur := flag.Duration("dur", 30*time.Minute, "window duration")
	out := flag.String("out", "brokerprobe.ndjson", "events file")
	flag.Parse()

	f, err := os.Create(*out)
	if err != nil {
		fmt.Fprintln(os.Stderr, "could not create file:", err)
		os.Exit(1)
	}
	defer f.Close()

	p := &probe{
		w:        bufio.NewWriter(f),
		start:    time.Now(),
		sysFirst: map[string]uint64{},
		sysLast:  map[string]uint64{},
	}
	p.enc = json.NewEncoder(p.w)
	defer p.w.Flush()

	// Its own client ID: the node's and the backend's are already in use,
	// and a duplicate would force exactly the takeover we came here to observe.
	opts := mqtt.NewClientOptions().
		AddBroker(*broker).
		SetClientID(fmt.Sprintf("ws-brokerprobe-%d", os.Getpid())).
		SetUsername(*user).
		SetPassword(*pass).
		SetCleanSession(true).
		// High keepalive on purpose: every own PINGREQ dirties
		// $SYS/broker/messages/received, which is one of the counters being
		// read. It doesn't affect publish/messages/received, but it keeps
		// that total clean too.
		SetKeepAlive(300 * time.Second).
		SetAutoReconnect(true).
		SetConnectTimeout(10 * time.Second)

	opts.OnConnect = func(c mqtt.Client) {
		fmt.Printf("[%s] connected to broker\n", ts(time.Now()))
		for _, t := range []string{*topic, "station/01/status", "$SYS/broker/#"} {
			if tok := c.Subscribe(t, 0, p.handle); tok.Wait() && tok.Error() != nil {
				fmt.Fprintf(os.Stderr, "subscription to %s failed: %v\n", t, tok.Error())
			}
		}
		fmt.Printf("[%s] subscribed to telemetry, status and $SYS/broker/#\n", ts(time.Now()))
	}
	opts.OnConnectionLost = func(_ mqtt.Client, err error) {
		fmt.Printf("[%s] connection lost: %v\n", ts(time.Now()), err)
	}

	c := mqtt.NewClient(opts)
	if tok := c.Connect(); tok.Wait() && tok.Error() != nil {
		fmt.Fprintln(os.Stderr, "could not connect:", tok.Error())
		os.Exit(1)
	}

	fmt.Printf("%s window — the node publishes every ~63 s, so that's ~%d cycles\n",
		*dur, int(dur.Seconds()/63))

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	select {
	case <-time.After(*dur):
	case <-sig:
		fmt.Println("\ninterrupted — shutting down and summarizing what was measured")
	}

	c.Disconnect(500)
	p.mu.Lock()
	p.w.Flush()
	p.mu.Unlock()
	p.report(*out)
}

func (p *probe) handle(_ mqtt.Client, m mqtt.Message) {
	now := time.Now()
	p.mu.Lock()
	defer p.mu.Unlock()

	ev := event{T: now.Format(time.RFC3339Nano), Topic: m.Topic()}
	payload := string(m.Payload())

	switch {
	case m.Topic() == "station/01/telemetry":
		ev.Bytes = len(m.Payload())
		var doc map[string]any
		if err := json.Unmarshal(m.Payload(), &doc); err == nil {
			ev.Doc = doc
			if v, ok := num(doc["boot_count"]); ok {
				ev.BootCount = &v
				p.bootCounts = append(p.bootCounts, v)
			}
			if v, ok := num(doc["rssi_dbm"]); ok {
				ev.RSSI = &v
			}
			if s, ok := doc["firmware"].(string); ok {
				ev.Firmware = s
				p.firmware = s
			}
		}
		p.telemetry++
		bc := int64(-1)
		if ev.BootCount != nil {
			bc = *ev.BootCount
		}
		fmt.Printf("[%s] TELEMETRY  boot=%d  %d B  rssi=%s  fw=%s\n",
			ts(now), bc, ev.Bytes, rssiStr(ev.RSSI), ev.Firmware)

	case strings.HasPrefix(m.Topic(), "$SYS/"):
		ev.Value = payload
		key := strings.TrimPrefix(m.Topic(), "$SYS/broker/")
		if v, err := strconv.ParseUint(strings.Fields(payload)[0], 10, 64); err == nil {
			if _, seen := p.sysFirst[key]; !seen {
				p.sysFirst[key] = v
			}
			prev, had := p.sysLast[key]
			p.sysLast[key] = v
			if watched(key) && (!had || prev != v) {
				delta := ""
				if had {
					delta = fmt.Sprintf("  (%+d)", int64(v)-int64(prev))
				}
				fmt.Printf("[%s] $SYS %-34s %d%s\n", ts(now), key, v, delta)
			}
			if key == "clients/connected" {
				p.connSamples = append(p.connSamples, connSample{at: now, n: v})
			}
		}

	default:
		ev.Value = payload
		fmt.Printf("[%s] %s  %s\n", ts(now), m.Topic(), truncate(payload, 120))
	}

	_ = p.enc.Encode(ev)
}

// The counters printed live. The rest still goes to the NDJSON.
func watched(key string) bool {
	switch key {
	case "publish/messages/received", "publish/bytes/received",
		"messages/received", "clients/connected", "clients/total",
		"clients/disconnected", "clients/maximum":
		return true
	}
	return false
}

func (p *probe) report(outPath string) {
	p.mu.Lock()
	defer p.mu.Unlock()

	elapsed := time.Since(p.start)
	pubIngested := p.delta("publish/messages/received")
	bytesIngested := p.delta("publish/bytes/received")

	fmt.Println("\n" + strings.Repeat("═", 78))
	fmt.Printf("SUMMARY — %s window   (firmware seen: %s)\n", elapsed.Round(time.Second), orDash(p.firmware))
	fmt.Println(strings.Repeat("═", 78))

	fmt.Printf("\nTelemetry DELIVERED to this subscriber : %d messages\n", p.telemetry)
	fmt.Printf("PUBLISH INGESTED by the broker ($SYS)  : %d", pubIngested)
	if pubIngested > 0 {
		fmt.Printf("   (%d B, ~%d B/msg)", bytesIngested, bytesIngested/pubIngested)
	}
	fmt.Println()

	// The verdict. Watch the window's edge: the $SYS counter can arrive up
	// to 10 s late relative to the last payload, so ±1 means nothing.
	diff := int64(pubIngested) - int64(p.telemetry)
	fmt.Println()
	switch {
	case diff > 1:
		fmt.Printf("⇒ %d PUBLISH messages entered the broker and were NOT delivered.\n", diff)
		fmt.Println("  The loss is ON THE BROKER SIDE (ingress or routing), not over the air.")
		fmt.Println("  Consistent with the duplicate client-ID takeover.")
	case diff < -1:
		fmt.Printf("⇒ %d MORE messages arrived than PUBLISH counted — check the window\n", -diff)
		fmt.Println("  (another active publisher, or a counter reset by a broker restart?)")
	default:
		fmt.Println("⇒ ingested ≈ delivered: everything that entered the broker was delivered.")
		fmt.Println("  The loss happens BEFORE the broker — the PUBLISH never arrived.")
		fmt.Println("  That rules out an internal drop and leaves the node→broker path (TCP/air).")
	}

	// Cycles the node actually lived, per boot_count: this is the real denominator.
	if len(p.bootCounts) >= 2 {
		bc := append([]int64(nil), p.bootCounts...)
		sort.Slice(bc, func(i, j int) bool { return bc[i] < bc[j] })
		span := bc[len(bc)-1] - bc[0] + 1
		var gaps []string
		for i := 1; i < len(bc); i++ {
			for m := bc[i-1] + 1; m < bc[i]; m++ {
				gaps = append(gaps, strconv.FormatInt(m, 10))
			}
		}
		fmt.Printf("\nboot_count %d..%d → %d cycles lived, %d delivered = %.0f%% lost\n",
			bc[0], bc[len(bc)-1], span, len(bc), 100*float64(span-int64(len(bc)))/float64(span))
		if len(gaps) > 0 {
			fmt.Printf("Gaps: %s\n", strings.Join(gaps, ", "))
		}
	}

	// Lifetime of the node's session inside the broker.
	fmt.Println("\n── clients/connected (takeover premise) ──")
	if len(p.connSamples) == 0 {
		fmt.Println("no samples")
	} else {
		lo, hi := p.connSamples[0].n, p.connSamples[0].n
		for _, s := range p.connSamples {
			if s.n < lo {
				lo = s.n
			}
			if s.n > hi {
				hi = s.n
			}
		}
		fmt.Printf("range %d..%d across %d transitions\n", lo, hi, len(p.connSamples))
		for i, s := range p.connSamples {
			d := ""
			if i > 0 {
				d = fmt.Sprintf("  (+%s since the previous one)", s.at.Sub(p.connSamples[i-1].at).Round(time.Second))
			}
			fmt.Printf("  [%s] %d%s\n", ts(s.at), s.n, d)
		}
		fmt.Println("\nReading: if the node's session closes on its own, connected drops a few")
		fmt.Println("seconds after each cycle. If it stays up between cycles, the session")
		fmt.Println("survives and every reconnection hits a still-alive client ID.")
	}

	fmt.Printf("\nRaw events: %s\n", outPath)
}

func (p *probe) delta(key string) uint64 {
	f, ok := p.sysFirst[key]
	if !ok {
		return 0
	}
	return p.sysLast[key] - f
}

func num(v any) (int64, bool) {
	if f, ok := v.(float64); ok {
		return int64(f), true
	}
	return 0, false
}

func ts(t time.Time) string { return t.Format("15:04:05") }

func rssiStr(v *int64) string {
	if v == nil {
		return "?"
	}
	return strconv.FormatInt(*v, 10)
}

func orDash(s string) string {
	if s == "" {
		return "—"
	}
	return s
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "…"
}

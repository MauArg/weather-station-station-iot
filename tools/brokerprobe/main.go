// brokerprobe — mide, desde un tercer consumidor, si los payloads que el nodo
// da por publicados llegan al broker.
//
// La pregunta que contesta: de los ciclos que el nodo registra como
// LOG_PUBLISH_OK y que no aparecen en ningún suscriptor, ¿el PUBLISH entró al
// broker y se perdió adentro, o nunca llegó?
//
// Un tercer suscriptor no alcanza para eso: si el broker descarta el mensaje en
// el ingreso, TODOS los suscriptores lo pierden igual. Lo que sí discrimina son
// los contadores $SYS, que Mosquitto incrementa en handle__publish, antes de
// rutear:
//
//   $SYS/broker/publish/messages/received  → PUBLISH ingresados
//   $SYS/broker/publish/bytes/received     → bytes, para confirmar que el
//                                            ingresado era la telemetría (~480 B)
//                                            y no un mensaje corto
//
// Y de paso, $SYS/broker/clients/connected da la vida de la sesión del nodo
// dentro del broker, que es la premisa de la hipótesis del takeover: si la
// sesión queda viva entre ciclos (~90 s con keepalive 60), cada reconexión
// fuerza un takeover por client-ID duplicado.
//
// Mosquitto publica cada valor de $SYS sólo cuando cambia, así que la secuencia
// de mensajes ES la secuencia de transiciones, con su timestamp.
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
	// sólo para telemetría
	BootCount *int64 `json:"boot_count,omitempty"`
	RSSI      *int64 `json:"rssi,omitempty"`
	Firmware  string `json:"firmware,omitempty"`
	Bytes     int    `json:"bytes,omitempty"`
	// El payload entero. La primera versión sólo extraía los cuatro campos de
	// arriba, y cuando hizo falta cruzar las pérdidas contra `system_v` —la
	// hipótesis de que la alimentación se cae en los picos de transmisión— el
	// dato había pasado por acá y no se había guardado. Guardar todo cuesta
	// nada y evita tener que repetir una ventana de 35 minutos.
	Doc map[string]any `json:"doc,omitempty"`
}

type probe struct {
	mu  sync.Mutex
	enc *json.Encoder
	w   *bufio.Writer

	start time.Time

	// telemetría entregada
	telemetry  int
	bootCounts []int64
	firmware   string

	// contadores $SYS (primer valor visto y último)
	sysFirst map[string]uint64
	sysLast  map[string]uint64

	// transiciones de clients/connected
	connSamples []connSample
}

type connSample struct {
	at time.Time
	n  uint64
}

func main() {
	broker := flag.String("broker", "tcp://192.168.18.250:1883", "URL del broker")
	user := flag.String("user", "weather_station_iot", "usuario MQTT")
	pass := flag.String("pass", "aXdC7nE2gLEe", "password MQTT")
	topic := flag.String("topic", "station/01/telemetry", "topic de telemetría")
	dur := flag.Duration("dur", 30*time.Minute, "duración de la ventana")
	out := flag.String("out", "brokerprobe.ndjson", "archivo de eventos")
	flag.Parse()

	f, err := os.Create(*out)
	if err != nil {
		fmt.Fprintln(os.Stderr, "no se pudo crear el archivo:", err)
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

	// Client ID propio: el del nodo y el del backend ya están en uso, y un
	// duplicado forzaría exactamente el takeover que vinimos a observar.
	opts := mqtt.NewClientOptions().
		AddBroker(*broker).
		SetClientID(fmt.Sprintf("ws-brokerprobe-%d", os.Getpid())).
		SetUsername(*user).
		SetPassword(*pass).
		SetCleanSession(true).
		// Keepalive alto a propósito: cada PINGREQ propio ensucia
		// $SYS/broker/messages/received, que es uno de los contadores que se
		// están leyendo. No afecta a publish/messages/received, pero de paso
		// deja el total limpio.
		SetKeepAlive(300 * time.Second).
		SetAutoReconnect(true).
		SetConnectTimeout(10 * time.Second)

	opts.OnConnect = func(c mqtt.Client) {
		fmt.Printf("[%s] conectado al broker\n", ts(time.Now()))
		for _, t := range []string{*topic, "station/01/status", "$SYS/broker/#"} {
			if tok := c.Subscribe(t, 0, p.handle); tok.Wait() && tok.Error() != nil {
				fmt.Fprintf(os.Stderr, "suscripción a %s falló: %v\n", t, tok.Error())
			}
		}
		fmt.Printf("[%s] suscripto a telemetría, status y $SYS/broker/#\n", ts(time.Now()))
	}
	opts.OnConnectionLost = func(_ mqtt.Client, err error) {
		fmt.Printf("[%s] conexión perdida: %v\n", ts(time.Now()), err)
	}

	c := mqtt.NewClient(opts)
	if tok := c.Connect(); tok.Wait() && tok.Error() != nil {
		fmt.Fprintln(os.Stderr, "no se pudo conectar:", tok.Error())
		os.Exit(1)
	}

	fmt.Printf("ventana de %s — el nodo publica cada ~63 s, así que son ~%d ciclos\n",
		*dur, int(dur.Seconds()/63))

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	select {
	case <-time.After(*dur):
	case <-sig:
		fmt.Println("\ninterrumpido — cerrando y resumiendo lo medido")
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
		fmt.Printf("[%s] TELEMETRÍA  boot=%d  %d B  rssi=%s  fw=%s\n",
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

// Los contadores que se imprimen en vivo. El resto va al NDJSON igual.
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
	fmt.Printf("RESUMEN — ventana de %s   (firmware visto: %s)\n", elapsed.Round(time.Second), orDash(p.firmware))
	fmt.Println(strings.Repeat("═", 78))

	fmt.Printf("\nTelemetría ENTREGADA a este suscriptor : %d mensajes\n", p.telemetry)
	fmt.Printf("PUBLISH INGRESADOS al broker ($SYS)    : %d", pubIngested)
	if pubIngested > 0 {
		fmt.Printf("   (%d B, ~%d B/msg)", bytesIngested, bytesIngested/pubIngested)
	}
	fmt.Println()

	// El veredicto. Ojo con el borde de la ventana: el contador $SYS puede
	// llegar hasta 10 s tarde respecto del último payload, así que ±1 no
	// significa nada.
	diff := int64(pubIngested) - int64(p.telemetry)
	fmt.Println()
	switch {
	case diff > 1:
		fmt.Printf("⇒ %d PUBLISH entraron al broker y NO se entregaron.\n", diff)
		fmt.Println("  La pérdida es DEL LADO DEL BROKER (ingreso o ruteo), no del aire.")
		fmt.Println("  Compatible con el takeover por client-ID duplicado.")
	case diff < -1:
		fmt.Printf("⇒ llegaron %d mensajes MÁS que PUBLISH contados — revisar la ventana\n", -diff)
		fmt.Println("  (¿otro publisher activo, o contador reiniciado por restart del broker?)")
	default:
		fmt.Println("⇒ ingresados ≈ entregados: todo lo que entró al broker se entregó.")
		fmt.Println("  La pérdida ocurre ANTES del broker — el PUBLISH nunca llegó.")
		fmt.Println("  Eso descarta el drop interno y deja al camino nodo→broker (TCP/aire).")
	}

	// Ciclos que el nodo vivió, según boot_count: es el denominador real.
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
		fmt.Printf("\nboot_count %d..%d → %d ciclos vividos, %d entregados = %.0f%% perdidos\n",
			bc[0], bc[len(bc)-1], span, len(bc), 100*float64(span-int64(len(bc)))/float64(span))
		if len(gaps) > 0 {
			fmt.Printf("Huecos: %s\n", strings.Join(gaps, ", "))
		}
	}

	// Vida de la sesión del nodo dentro del broker.
	fmt.Println("\n── clients/connected (premisa del takeover) ──")
	if len(p.connSamples) == 0 {
		fmt.Println("sin muestras")
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
		fmt.Printf("rango %d..%d en %d transiciones\n", lo, hi, len(p.connSamples))
		for i, s := range p.connSamples {
			d := ""
			if i > 0 {
				d = fmt.Sprintf("  (+%s desde la anterior)", s.at.Sub(p.connSamples[i-1].at).Round(time.Second))
			}
			fmt.Printf("  [%s] %d%s\n", ts(s.at), s.n, d)
		}
		fmt.Println("\nLectura: si la sesión del nodo se cierra sola, connected baja pocos")
		fmt.Println("segundos después de cada ciclo. Si se queda arriba entre ciclos, la")
		fmt.Println("sesión sobrevive y cada reconexión pega contra un client-ID vivo.")
	}

	fmt.Printf("\nEventos crudos: %s\n", outPath)
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

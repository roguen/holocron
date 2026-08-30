# Reaching the control pages by name

Status: **normative for the Holocron side, and a runbook for the rest.**
`holocron-pc.aero4ge.com` is the theater PC and `holocron-shield.aero4ge.com` is
the Shield.

The control page is the phone-browser page that switches crystals and toggles the
overlays. It was reachable only as `http://192.168.68.54:32500/control`, which is
an address to remember, a port to remember, and a thing that breaks silently when
a lease moves.

This follows the pattern `cantina.aero4ge.com` established — a DNS-only A record
under a zone the site controls, and a Let's Encrypt certificate issued over
DNS-01 so nothing is ever exposed to the internet. **It departs from that pattern
in one way that is forced rather than chosen**, and the rest of this file is
mostly about why.

## Holocron does not terminate TLS, and on the Shield it cannot

Cantina's Barkeep loads a certificate and serves it. Holocron cannot do the same,
for three reasons in increasing order of severity.

**cpp-httplib is built without OpenSSL, deliberately.** `vcpkg.json` sets
`default-features: false` and says why: Companion is plain HTTP on the LAN, and
pulling OpenSSL in for a server that will never terminate TLS is a large
dependency and a licence exception for nothing. That is a decision, and it could
be reversed.

**The control page shares one server and one port with the Plex Companion API.**
`/control` is a route on the same `httplib::Server` that answers `/resources`,
`playMedia` and the timelines. Plex Companion is plain HTTP; putting TLS on that
listener would stop the thing being a cast target, which is the whole product.
That could be worked around with a second listener.

**An Android app cannot bind port 443.** Ports below 1024 need root, and Holocron
on the Shield is an ordinary app. This one cannot be worked around at all: a bare
`https://holocron-shield.aero4ge.com` with no port in it can never be served by
the process that serves the control page.

So TLS is terminated **in front of** Holocron. That is a genuine departure from
this network's habit, where each service holds its own certificate — `unifi-core`
serves its own, QTS serves its own through `stunnel`, Barkeep loads its own — and
it is worth stating plainly rather than letting the next person discover it.

## What that makes the DNS records point at

**The proxy, not the players.** This is the consequence that surprises, and it
is the one to check first when something does not work:

| Name | Resolves to | Reaches |
|---|---|---|
| `holocron-pc.aero4ge.com` | the reverse proxy | `192.168.68.54:32500/control` |
| `holocron-shield.aero4ge.com` | the reverse proxy | `192.168.68.38:32550/control` |

`cantina.aero4ge.com` points at the machine that answers. These do not.

**Plex still uses the addresses and knows nothing about any of this.** GDM
announces an address and a port, controllers use what is announced, and the proxy
has never heard of the Companion protocol. Nothing about these names may ever be
fed to a controller.

## The reservations are load-bearing, and both already exist

A public name over a DHCP lease breaks silently when the lease moves — the same
warning Cantina's own document carries.

| Host | Address | MAC | Reservation |
|---|---|---|---|
| Theater PC, `HOME-Griffen-PC` | `192.168.68.54` | `24:4b:fe:81:13:4b` | already pinned |
| NVIDIA Shield | `192.168.68.38` | `74:25:54:04:c1:f3` | already pinned |

Both confirmed 2026-08-29 against the network inventory and **corroborated from
the devices themselves** rather than only from the controller that records them —
the MAC was read off each machine's own interface and matched.

**The theater PC is `192.168.68.54` and was `192.168.68.144` until 2026-08-29.**
Holocron's own documents still said `.144`, which by then answered nothing. It is
also the machine Cantina calls the theater PC, so `cantina.aero4ge.com` and
`holocron-pc.aero4ge.com` are two names for one host.

## What Holocron contributes

Almost nothing, and that is the design. `[plex] control_url` changes **what the
startup line prints** and nothing else:

```toml
[plex]
control_url = "https://holocron-pc.aero4ge.com"
```

```
holocron: control page at https://holocron-pc.aero4ge.com/control
```

Empty is the default and keeps the old behaviour exactly. It does not change what
the process binds, what it announces, or what any controller is told — see
`include/holocron/control_url.hpp`, which is also where the trailing-slash,
already-has-`/control` and missing-scheme corrections live, with tests.

**Holocron does not verify the name.** It cannot see the DNS record, the proxy or
the certificate, and a check against any of them would be wrong the moment the
network changed underneath it. Being wrong here costs one line of terminal output
on a machine in another room, which is the cheapest failure available.

## The runbook, in order

The order matters: each step is only safe once the one above it is true.

**1. Confirm the reservations.** Done, above. Re-check if either host is ever
re-imaged, because a new NIC is a new MAC.

**2. Give the proxy an address, and pin it.** The NAS answers 443 on all four of
its host interfaces because QTS binds them all, so the proxy needs an address of
its own rather than a spare NIC. Containers on that box routinely hold their own
LAN addresses; take one, and **reserve it before the DNS records exist** — a
proxy on an unpinned lease has exactly the failure this whole section is about.

**3. Two A records in Cloudflare, DNS-only.** Both pointing at the proxy's
address. Unproxied, because an RFC1918 address behind Cloudflare's proxy does not
work. Publicly resolvable is not publicly reachable, and the consequence — the
internal addressing is queryable and the names appear in Certificate Transparency
logs — is the same one already accepted for `nas`, `unifi` and `cantina`.

**4. One certificate covering both names**, from the `acme.sh` container already
running on the NAS at `/share/CACHEDEV1_DATA/Container/acme/`, over DNS-01 with
the Cloudflare token it already holds. Two names on one certificate rather than
two certificates, because they renew together and are served by one process.

**5. The proxy config.** Route by `Host`, terminate TLS, forward to the two
addresses above. Nothing clever is needed: no path rewriting, no WebSocket, no
buffering concerns — the control page is plain form posts with a 303 back.

**6. Set `control_url` on each box** and restart the player, so the printed line
matches reality.

## What this does not do, and should be said out loud

**Nothing watches the renewal.** The same gap Cantina records against its own
certificate, with the same shape: the theater works perfectly until a day weeks
later when the browser refuses. Holocron has no health endpoint reporting
certificate expiry and adding one would not help, because the certificate is not
Holocron's — it belongs to a proxy the player cannot see.

**The proxy is a new dependency for the control page.** If it is down, the names
stop working and the addresses still do. Worth knowing before standing in a dark
theater concluding the player is broken:
`http://192.168.68.38:32550/control` is the fallback and always will be.

**The Shield's port is `32550`, not `32500`.** The Plex player app holds 32500 on
that box and Holocron cannot reliably have it (D-058, issue 247). The proxy has
to know that, and it is the sort of detail that produces a 502 nobody can explain.

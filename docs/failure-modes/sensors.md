# Failure modes: ADS1115 current sensing (firmware)

`failure -> detection -> recovery` for the ADS1115 inputs, CT current and DC sensors (`lib/thesada-mod-ads1115/`). A blank recovery cell is a gap.

| Failure | Detection | Recovery |
|---------|-----------|----------|
| Chip missing at boot | `ads.device_missing addr=` (`addDevice`), `module.status` lists the address with `!` | Probed every read cycle. `ads.device.state_change addr= from=offline to=online` on the first full read, no reboot |
| Chip drops off the bus (loose connector, NACK) | `ads.device.state_change addr= from=online to=offline reason=nack` (`readChannelRms`), `sensors current` prints `device 0x.. offline`. A drop during sampling times out a conversion, and the re-probe after it reports `nack` | Its channels leave `<prefix>/sensor/current` until a probe ACKs and a full read succeeds. Each wait is bounded, so the task watchdog does not fire |
| Chip ACKs but a conversion never finishes | `reason=timeout` after 10 ms on one conversion (`sampleRms`) while the re-probe still ACKs | Same as a NACK |
| `gain` between two PGA ranges | `ads.gain_rounded name= requested= fs=` at boot (`loadChannels`) | Reads correctly at the rounded-up range. A standard gain silences the warning |
| CT biased to mid-rail, zero load | none needed | The DC mean is removed, so it reads 0 A, not its bias |
| Bare CT on a single-ended mux (`A0_GND`) | `ads.clipped name= reason=single_ended_no_bias` once a swing touches ground, `ads.clip_cleared name=` once one clears it (`markClipped`). Readings sit at about 0.55x of true | Wiring fix: a differential pair, or a mid-rail bias |
| DC sensor (pressure transducer) without `"dc": true` | none, reads about 0 V at any level. `ads.channel ... coupling=ac` at boot | Set `"dc": true` on the channel, which reads the mean |

## Gaps

- **HA keeps the last current and power of an offline chip.** The per-sensor topics stop, and the ADS1115 discovery in `MQTTClient.cpp::publishDiscovery` sets no `expire_after`.
- **Offline is not on a status topic.** Only the log, `module.status` and `sensors current` show it.
- **Channels under `ads1115.devices[]` get no HA discovery.** `publishDiscovery` reads only the scalar `ads1115.channels` list.

---

Related: [`../invariants.md`](../invariants.md) (Sensors), [`mqtt.md`](mqtt.md).

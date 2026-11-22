# Analog audio driver (`pwmaudio`)

Audio output on the **3.5 mm jack of the Raspberry Pi 1** (Model B; the B+ and A+ use the same pins
and should work too, untested). The jack has no codec: the two BCM2835 **PWM** channels drive it
through an RC filter, and the driver feeds the PWM from a **DMA** channel paced by the PWM's DREQ.
The driver is `pwmaudio.dll`, a PDD under Microsoft's wave MDD; CE 7's wave API (`waveapi.dll`)
sits on top and mixes all applications' streams in software.

> Build/boot mechanics for the whole BSP live in `CLAUDE.md`. The Pi Zero has no jack (see Future
> work).

## Current state

Verified on a Pi 1 Model B.

| Area | State |
|---|---|
| PWM clock, GPIO40/45, PWM + DMA ring, DMA interrupt (`SYSINTR_AUDIO`) | Done |
| Left/right order, sample rate (spectrum analyser + timing) | Done |
| `waveOut` playback, PCM 8/16-bit, mono/stereo, 4-48 kHz | Done |
| System sounds, `PlaySound`, TCPMP (also while playing video) | Done |
| Several applications at once | Done (mixed by waveapi's software mixer) |
| Volume (`waveOutSetVolume`, left/right) | Done (software gain) |
| TPDF dither + first-order noise shaping | Done (registry `Dither`, default 2) |
| Recording (wave input) | Not available: no input hardware |
| Power management | Minimal: DMA stopped on power-down (suspend is a halt on this BSP) |

## Enabling it

- OS design: **Waveform Audio** (`SYSGEN_AUDIO`; the sample design also has DirectShow
  `sysgen_dshow_waveout`).
- BSP catalog items: **Analog audio (Pi 1 3.5 mm jack)** (`BSP_AUDIO=1`, the driver) and optionally
  **Audio test (audiotest.exe)** (`BSP_AUDIOTEST=1`, test app with a desktop shortcut).
- `platform.reg` changes need **Copy Files to Release Directory** before makeimg, and with
  `BSP_SDROOT` the registry hive on the card must be deleted (`\Windows\Registry`) for new values to
  appear (see `docs/sdroot.md`).

## Hardware

| Item | Value |
|---|---|
| Pins | GPIO40 = PWM0 = channel 1 = **left**; GPIO45 = PWM1 = channel 2 = **right**; both ALT0 |
| PWM block | `0x2020C000` (bus `0x7E20C000`); FIFO input `FIF1` at +0x18 |
| PWM mode | both channels enabled, PWM (balanced, `MSEN`=0) mode, `USEF` (FIFO) and `RPTL` (repeat last word when the FIFO is empty) |
| PWM clock | clock manager `CM_PWMCTL`/`CM_PWMDIV` (`0x201010A0`/`A4`, password `0x5A`): source PLLD (500 MHz), divider 2 = **250 MHz** |
| PWM DMA | `DMAC` = enable, panic 7, DREQ 3 |
| DMA | channel **4** (`AUDIO_DMA_CHANNEL` in `bsp_cfg.h`), DREQ (`PERMAP`) **5**, destination `0x7E20C018`, source increment, wait for write response, interrupt per block |
| DMA bus address | physical + `0x40000000` (registry `DmaBusOffset`, same as `dwchci`); buffer from `AllocPhysMem` uncached, so no cache maintenance |
| Interrupt | DMA channel 4 = GPU IRQ 20 (IRQ1 bit 20), static `SYSINTR_AUDIO` = 19 (`SYSINTR_FIRMWARE`+3). The OAL ISR masks it and returns `SYSINTR_AUDIO`; `OEMInterruptDone` unmasks it |

Verified facts learned during bring-up:

- **One FIFO word per PWM period.** With both channels on the FIFO the PWM takes one word per period
  and the channels alternate, so a stereo frame lasts **two** periods:
  `frame rate = clock / (2 * range)`, `range = clock / (2 * rate)` = **2834** at 44.1 kHz (about
  11.5 bits). With `range = clock / rate` everything played at half speed, an octave low (1 s of
  samples took 2 s with no late interrupts). Measured fix: 44 interrupts of 1024 frames in 1022 ms,
  correct pitch on a spectrum analyser, no FIFO gaps.
- **GPIO40 is the left channel** (checked by ear).
- **The PWM does not work at 500 MHz** (PLLD / 1): it never drains its FIFO (`STA`=`0x601`: FIFO full,
  both channels "transmitting") and the jack buzzes. 250 MHz is the maximum we use.
- **Clearing the DMA interrupt:** write `CS` = `ACTIVE | INT | END`. Writing `ACTIVE`=0 pauses the
  channel.
- **IST priority.** At the default thread priority (251) the refill thread missed deadlines while
  `smsc95xx`/NDIS printed heavily over KITL during boot (crackle). Priority **110** (registry
  `Priority256`, also used by the MDD for its IST) is above the Ethernet data threads (130) and
  below the USB IST (101), and fixed it.
- **Failure path.** A PWM left enabled after a failed start (for example the 500 MHz attempt) keeps
  buzzing. `PwmHwDeinit` disables the PWM DMA request, both channels and the PWM clock.

## Software design

```
applications (waveOut, PlaySound, DirectShow)
  -> waveapi.dll + software mixer (swmixer): mixes all streams, opens the device at 44.1 kHz 16-bit stereo
    -> pwmaudio.dll
         wavemdd.lib   Microsoft wave MDD (prebuilt, linked from the public tree, not copied):
                       WAV_* entry points, WAVEHDR queue, IST on SYSINTR_AUDIO
         wavepdd.cpp   our PDD: formats, volume, dither, write cursor, MDD state handling
         pwmhw.cpp     our render engine: clock, GPIO, PWM, DMA ring, read position
```

Files: `src\drivers\pwmaudio` (`pwmhw.h/.cpp`, `wavepdd.cpp`, `pwmaudio.def` exporting the MDD's
`WAV_*`). OAL: `src\INC\bsp_cfg.h`, `src\oal\oallib\intr.c`, register definitions in
`src\INC\BCM2835.h`. Test app: `src\apps\audiotest`.

### The software mixer in front of the driver

CE 7's wave API has a software mixer (`swmixer`, in `waveapi.dll`). Every open the driver sees is
44100 Hz 16-bit stereo, also for 11/22 kHz system sounds, and several applications can play at once
although the MDD supports only one stream. So no multi-stream front end of our own is needed. Its
settings are under `HKLM\Audio\SoftwareMixer` (`SampleRate`, `BufferSize`, `Buffers`,
`EnableLowPassFilter`, `Priority256`; names taken from `swmixer.lib`, defaults unknown, not set by the
BSP). The mixer recycles a few small buffers and sends the next one when the driver returns one
(`WOM_DONE`). That shaped the design below.

### DMA ring and write cursor

The DMA loops forever over a ring of **16 blocks x 256 frames** (4096 frames, ~93 ms at 44.1 kHz),
one control block per block, interrupt per block (~5.8 ms). A frame is two DWORDs in FIFO order
(left, right); each word is a duty value `0..range-1`, `range/2` is silence.

- When a block has finished, `PwmHwAck` clears it to silence. Anything nobody writes plays as
  silence, and stale audio never repeats unless the IST is more than a whole ring (~93 ms) late.
- `PwmHwReadPos` gives the absolute frame the DMA is reading, from the channel's `SOURCE_AD`
  register. The PDD writes audio at an absolute write cursor that stays at least
  `PWMHW_WRITE_MARGIN` (64) frames ahead of it, up to one ring ahead of the playing block.
- A header is completed **when it has been copied** (its `dwBytesRecorded` advances as bytes are
  copied; the MDD returns headers whose `dwBytesRecorded` reached `dwBufferLength` on the next
  interrupt). The mixer gets its buffers back within ~6 ms and the ring is never padded with silence
  while data exists.
- When the MDD has no more data it sends `WPDM_ENDOFDATA`: the DMA keeps running until the read
  position passes the write cursor ("draining"), then stops. `WPDM_CLOSE` waits for that (at most
  500 ms), so the end of a sound is not cut off. New data during the drain is appended.
- When the DMA stops, the PWM repeats the last word (mid-level), so there is no click. At driver load
  the outputs ramp from 0 to mid-level over ~23 ms.

History (why it is built this way): the first version completed headers only after they had played
and filled whole 1024-frame blocks at once, padding with silence. With the mixer's few small buffers
the ring settled into alternating audio and silence: playback at full speed but silent half the
time.

### Sample conversion

Per frame: read the source sample (byte-wise, so odd-aligned application buffers are fine; 8-bit is
unsigned), apply the left/right volume, then requantise from 16 bits to the PWM range (registry
`Dither`, read at driver load):

| `Dither` | Method |
|---|---|
| 0 | truncate |
| 1 | TPDF dither (+-1 PWM step, from a 32-bit LCG's top bits), round |
| 2 (default) | TPDF dither + first-order error-feedback noise shaping (noise transfer `1 - z^-1`, error clamped to +-2 steps, reset per stream) |

The arithmetic is in Q16 PWM steps and stays inside 32-bit signed integers for every supported rate.

## Registry (`HKLM\Drivers\BuiltIn\PwmAudio`)

| Value | Default | Meaning |
|---|---|---|
| `SysIntr` | 0x13 | `SYSINTR_AUDIO` |
| `DmaBusOffset` | 0x40000000 | physical to VideoCore bus address offset |
| `Priority256` | 110 (0x6E) | IST priority (MDD and test tone) |
| `Dither` | 2 | requantisation mode, see above |
| `TestTone` | 0 | 1 = at load play 0.5 s 440 Hz left, then 0.5 s 880 Hz right, and log timing, interrupts and PWM gap/error flags |

## Testing

- `TestTone=1`: expect `test tone done: ~172 DMA interrupts in ~1000 ms ... ` and
  `PWM status during playback ...: no gaps or errors`.
- `audiotest.exe` (desktop shortcut "Audio Test") plays: 440 Hz left, 880 Hz right (44.1 kHz
  stereo), 660 Hz at 22.05 kHz mono, 330 Hz at 11.025 kHz 8-bit mono, 523 Hz with a 0.5 s pause, and a
  5 s 440 Hz fade from -20 dB to -80 dB (to hear requantisation distortion and hiss). It reports the
  time per test and the final `waveOutGetPosition`. `audiotest <file.wav>` plays a file with
  `PlaySound`.
- The driver logs `PWMAUD: open <format>` per stream and `PWMAUD: close, N underruns ...` when audio
  arrived after the DMA had passed (a few around stream starts are harmless).

## Limitations

- About 11.5 bits at 44.1 kHz (range 2834); the Pi 1 jack is also noisy for analog reasons.
- `waveOutGetPosition` counts copied bytes: up to ~93 ms ahead of what is heard.
- `waveOutPause` drops the audio already copied but not yet played (up to ~93 ms).
- The MDD supports one stream; this relies on waveapi's mixer for several applications.
- No input device.

## Future work

- **Pi Zero:** no jack, but PWM0/PWM1 can be routed to GPIO12/13 or GPIO18/19 (ALT0 / ALT5) with an
  external RC filter. Would need the pins as a registry option. Untested.
- Stop the PWM clock while idle (small power saving; restarting may click).
- Report the played position (read position) instead of the copied position, if A/V sync needs it.

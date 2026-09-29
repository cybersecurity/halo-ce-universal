# macOS guest audio status

The host audio adapter uses SDL3 `SDL_OpenAudioDeviceStream` and translates the
guest's 12-byte ILP32 `SDL_AudioSpec` explicitly. A dedicated guest producer
worker attaches musl TLS and fills at most 40 ms of input PCM in 10 ms chunks.
The DirectSound guest callback retains its userdata/stream handle and advances
its buffer cursors by the requested frame count; production runs up to 40 ms
ahead of physical playback. The SDL device callback only notifies the worker
with a nonblocking trylock, so it never waits for guest mixing or scheduling.
PCM submission occurs on the worker outside SDL's device callback and outside
the binding mutex. Resume fills the initial lead before starting playback.
Shutdown pauses its logical stream, disables callbacks, joins the producer,
then destroys the stream and releases its guest stack.

Audio is opt-in while game startup is being validated:

```sh
HALO_AUDIO_ENABLE=1
```

`halo_host_sdl_audio_test` validates S16LE stereo 22.05 kHz, guest stack
arguments/TLS, bounded production, and shutdown during a deliberately slow
100 ms guest callback. A second logical movie stream continues receiving
callbacks during the delay and after the guest stream is destroyed; its own
pause/resume is also tested. The old synchronous bridge fails that shared
device responsiveness check. The updated bridge and test pass with SDL's dummy
driver and AddressSanitizer. The fixture submits silent PCM and does not prove
subjective game or movie sound quality.

`HALO_AUDIO_EVIDENCE=1` now measures F32 sample validity, peak/RMS level,
callback duration, callbacks exceeding their requested audio duration, and
short fills. The ordinary menu run `ordinary-menu-audio14` submitted 2,793,472
finite samples at 48 kHz stereo with no samples above full scale; peak was
0.867825 and RMS was 0.118513. Its longest callback took 0.464 ms against a
4096-byte request duration of about 10.7 ms. It recorded no late callbacks,
short fills, or worker errors. This validates the menu's decoded float output
and bridge timing; it does not establish subjective music quality or smooth
audio during campaign loading.

During actual intro run `bink-complete23`, the old synchronous bridge stalled
47.740 ms: producer wake delay accounted for 47.728 ms, whereas the largest
guest mixer invocation was 2.666 ms. This motivated the asynchronous producer.
Its updated evidence reports physical callback duration, producer wake and
invoke time, input queue maximum/bound, and device requests that lacked queued
data. Actual intro audio continuity and final movie drain require a fresh run.

Actual `intro-timer24` validated the async bridge: 4,238,016 finite game-mixer
samples, no values above full scale, no short fills or device requests lacking
queued PCM, and at most 15,360 bytes of queued 48 kHz stereo F32 input (40 ms).
The longest physical SDL callback took 0.012 ms; producer wake delay reached
26.114 ms and guest mixing reached 1.289 ms. The independent movie decoder
submitted all 1,542,270 source PCM samples and closed with queued/available
bytes both zero, no drain timeout, and all 481 video frames presented. The
video still started slowly and caught up later; this audio timing evidence
does not establish smooth intro video or subjective listening quality.

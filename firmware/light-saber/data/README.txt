Light-Saber Sound Files
=======================

Place WAV files in this directory, then upload to the ESP32-S3 with:
  pio run --target uploadfs

Required format:
  - 16-bit PCM (uncompressed)
  - Mono (single channel)
  - 22050 Hz sample rate

Expected files:
  hum.wav      - Idle hum loop (~1-2 sec, seamless loop)
  ignite.wav   - Power-on ignition sound (~1 sec)
  retract.wav  - Power-off retraction sound (~1 sec)
  clash.wav    - Impact/hit sound (~0.5 sec)
  swing.wav    - Swing whoosh sound (~0.5 sec)

If hum.wav is missing, the firmware uses a procedural
120Hz synthesized hum as a fallback. Other missing files
are simply skipped (no sound plays for that event).

Converting with ffmpeg:
  ffmpeg -i input.mp3 -ar 22050 -ac 1 -sample_fmt s16 -acodec pcm_s16le output.wav

Converting with Audacity:
  1. Open audio file
  2. Tracks > Mix > Mix Stereo Down to Mono
  3. Tracks > Resample... > 22050 Hz
  4. File > Export Audio > WAV (Microsoft) 16-bit PCM

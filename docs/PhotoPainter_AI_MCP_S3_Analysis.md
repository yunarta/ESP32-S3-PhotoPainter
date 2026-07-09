# PhotoPainter AI, Voice, MCP, dan AWS S3 Analysis

Dokumen ini merangkum analisa besar-besaran atas arsitektur firmware ESP32-S3-PhotoPainter, terutama Mode 3/Xiaozhi, voice pipeline, AI image generation, MCP tools, dan rencana penambahan MCP untuk download gambar dari AWS S3.

## Ringkasan keputusan

- Mode 3 saat ini adalah mode Xiaozhi/voice assistant yang memakai server-side STT/LLM/TTS dan firmware sebagai client audio Opus + MCP.
- Firmware tidak generate voice assistant secara lokal; device hanya encode mic ke Opus, kirim ke server, menerima Opus audio response, decode ke PCM, lalu play ke speaker.
- Suara lokal yang ada hanyalah pre-recorded OGG/Opus prompt/sound effect dan prompt mode selection dari data lokal.
- Mengganti LLM saja hanya bisa murah jika server voice lama mendukung custom LLM/proxy; kalau tidak, perlu mengganti satu pipeline STT/LLM/TTS/protocol bridge dan cost audio bisa bertambah.
- Untuk AWS S3, rekomendasi terbaik adalah MCP tool yang menerima presigned URL atau generic image URL, bukan implement AWS SDK/Signature V4 di ESP32.
- MCP `self.disp.open_image_url` lebih fleksibel daripada tool khusus `bucket/key`, karena bisa dipakai untuk S3 presigned URL, CloudFront, backend URL, CDN, dan image generation output.

## Struktur mode firmware

Entry point firmware ada di `app_main()`. Firmware membuka NVS namespace `PhotoPainter` dan membaca/menulis beberapa key utama:

- `NetworkMode`
- `PhotPainterMode`
- `Mode_Flag`

Setelah init umum via `User_Mode_init()`, nilai `PhotPainterMode` menentukan mode aktif:

- `0x01`: Basic mode
- `0x02`: Network mode
- `0x03`: Xiaozhi mode / Mode 3
- `0x04`: Mode Selection

Mode selection memakai GPIO4 untuk memilih mode 1/2/3, lalu menyimpan pilihan ke NVS dan restart. Jadi Mode 3 bukan fitur kecil terisolasi; Mode 3 masuk ke `Application::Start()` dan mengaktifkan stack Xiaozhi.

## Init hardware umum

`User_Mode_init()` membuat dan menginisialisasi resource global yang dipakai semua mode:

- `SDPort`
- `decdither`
- `ePaperDisplay`
- `I2cBus`
- `epaper_gui_semapHandle`
- `epaper_groups`
- LED event groups
- button tasks
- charging monitor task

Render e-paper diproteksi mutex `epaper_gui_semapHandle`, sehingga tool MCP sebaiknya tidak menggambar langsung dari callback. Pola aman adalah set event bit, lalu render dilakukan task e-paper.

## Mode 1: Basic mode

Basic mode adalah mode photo frame low-power dari SD card:

1. Scan folder `/sdcard/06_user_foundation_img`.
2. Button click menampilkan gambar berikutnya.
3. Setelah display, device masuk deep sleep.
4. Wake bisa dari timer atau tombol.

Mode ini juga membaca config AI foundation untuk timer, meskipun naming class-nya `BaseAIModel`.

## Mode 2: Network mode

Network mode menangani AP/STA dan web upload:

- AP default untuk konfigurasi/upload.
- STA jika credential valid di NVS.
- Web server menerima gambar dan menyimpan ke `/sdcard/02_sys_ap_img/user_send.bmp`.
- Event dari server task memicu render ke e-paper.

## Mode 3: Xiaozhi + MCP + AI image

Mode 3 dijalankan ketika `PhotPainterMode == 0x03`. Board `waveshare_PhotoPainter` melakukan:

1. Init codec I2C.
2. `User_xiaozhi_app_init()`.
3. Init boot button.
4. Register MCP tools.

MCP tools custom PhotoPainter saat ini meliputi:

- `self.disp.SwitchPictures`
- `self.disp.getNumberimages`
- `self.disp.aiIMG`
- `self.disp.imgloop`
- `self.disp.imgloopEit`
- `self.disp.imgsetTimerloop min`
- `self.disp.imgsetTimerloop h`
- `self.disp.isSHTC3`

Tool-tool ini mostly hanya mengubah global state atau event group. Misalnya `self.disp.aiIMG` set bit `ai_IMG_Group`, lalu task AI image yang melakukan proses generation/download/render.

## AI image generation saat ini

Mode 3 membuat `BaseAIModel`, membaca config dari `/sdcard/06_user_foundation_img/config.txt`, dan melakukan init model dengan:

- model
- URL endpoint
- API key

Prompt user disimpan oleh `xiaozhi_ai_Message()` ke buffer global `str_ai_chat_buff`. Ketika tool `self.disp.aiIMG` dipanggil, `ai_IMG_Task()` melakukan:

1. Clear slideshow.
2. `AiModel->BaseAIModel_SetChat(chatStr)`.
3. `AiModel->BaseAIModel_GetImgName()`.
4. Jika sukses, set bit `epaper_groups` untuk render AI image.

`BaseAIModel_SetChat()` membangun request JSON untuk image generation dengan field seperti:

- `model`
- `prompt`
- `response_format: "url"`
- `size: "1280x768"`
- `stream: false`
- `watermark: false`

`BaseAIModel_GetImgURL()` POST ke endpoint AI dan parse `data[0].url`. Lalu `BaseAIModel_DownloadImgToPsram()` download image URL ke PSRAM. Hasilnya disimpan ke `/sdcard/04_sys_ai_img/sys_ai.jpg`, lalu e-paper render file tersebut.

## Voice pipeline

Voice assistant tidak digenerate di device. Firmware hanya melakukan streaming audio.

Flow utama:

```text
Mic PCM
  -> audio processor
  -> Opus encoder
  -> protocol send queue
  -> server
  -> STT / LLM / TTS di server
  -> Opus audio packets
  -> Opus decoder
  -> PCM playback queue
  -> speaker
```

Di firmware ada dua jalur teknis:

1. Audio path: Opus packets.
2. Control/inference path: JSON messages.

Dalam conversation user experience, keduanya menjadi satu flow.

### WebSocket

Jika memakai WebSocket:

- binary frame = Opus audio
- text frame = JSON control/MCP events

### MQTT

Jika memakai MQTT:

- MQTT = JSON/control
- UDP encrypted = Opus audio

## Local audio yang benar-benar lokal

Yang lokal di device bukan TTS fleksibel, tapi:

- pre-recorded OGG/Opus sound effects seperti success/popup/alert/activation
- prompt mode selection dari `CodecPort`
- wake word detection lokal

`PlaySound()` membaca OGG/Opus asset lokal, parse `OggS`, `OpusHead`, `OpusTags`, lalu memasukkan packet Opus ke decode queue.

## Implikasi jika mengganti LLM

Firmware audio pipeline bisa tetap dipakai, tetapi TTS lama belum tentu bisa ikut dipakai jika server lama tightly-coupled dengan LLM lama.

Kemungkinan skenario:

1. Server lama mendukung custom LLM endpoint.
   - Paling bagus.
   - STT/TTS/protocol lama tetap dipakai.
   - LLM diarahkan ke proxy baru.
2. Server lama tidak mendukung custom LLM.
   - Perlu backend bridge sendiri.
   - Bridge harus handle STT, LLM, TTS, Opus protocol, dan MCP.
   - Cost audio tambahan muncul.

Kesimpulan: mengganti LLM saja murah hanya jika ada layer server yang bisa dipasang LLM baru sambil tetap mempertahankan STT/TTS/protocol lama.

## Codex, Claude, OpenAI, dan image/voice

- Codex cocok untuk coding/orchestration, bukan runtime voice generator atau image generator utama.
- Claude cocok untuk reasoning/tool calling/vision input/text output, tetapi bukan TTS engine native untuk device.
- OpenAI Realtime cocok untuk low-latency voice agent, tetapi idealnya lewat backend bridge.
- OpenAI image atau image model lain cocok untuk generate gambar, tetapi hasil akhirnya sebaiknya diproses backend dan disajikan ke ESP32 sebagai URL/file yang siap render.

## Cost strategy

Agar cost tidak membengkak:

- Jangan switch semua voice pipeline dulu.
- Pertahankan voice pipeline existing jika mungkin.
- Gunakan LLM baru hanya untuk task kompleks.
- Gunakan rule-based parser untuk command sederhana.
- Gunakan local OGG phrase untuk feedback pendek.
- Cache image generation output.
- Simpan hasil image ke SD card/S3 agar tidak generate ulang.

## MCP AWS S3 recommendation

Penambahan MCP untuk S3 sangat masuk akal, tapi sebaiknya bukan AWS SDK di firmware.

### Jangan lakukan ini

```text
self.disp.download_s3(bucket, key, access_key, secret)
```

Alasannya:

- AWS credentials rawan bocor jika disimpan di ESP32.
- Private S3 access membutuhkan AWS Signature Version 4.
- Signature V4 perlu canonical request, HMAC-SHA256 berlapis, timestamp valid, region, service, header canonicalization, dan session token handling.
- AWS SDK terlalu berat untuk use case simple di ESP32.
- TLS CA chain dan memory download image besar bisa menjadi masalah tambahan.

### Lakukan ini

```text
self.disp.open_image_url(url)
```

Backend membuat S3 presigned URL. ESP32 hanya melakukan HTTPS GET.

Keuntungan:

- AWS secret tetap di backend.
- URL bisa short-lived, misalnya 5-15 menit.
- URL hanya memberi akses ke object tertentu.
- Firmware tetap sederhana.
- Tool bisa dipakai untuk S3, CloudFront, image CDN, backend URL, atau output image generation.

## MVP MCP S3/image URL

Tool yang direkomendasikan:

```text
self.disp.open_image_url
```

Argument:

```json
{
  "url": "https://example-presigned-url-or-cdn-image"
}
```

Behavior:

1. Validasi URL basic.
2. Download image dari URL.
3. Save ke `/sdcard/04_sys_ai_img/remote.jpg`.
4. Set global `remote_img_path`.
5. Trigger event bit baru di `epaper_groups`.
6. `gui_user_Task()` render `remote_img_path` ke e-paper.

## Storage strategy

### MVP

Simpan overwrite ke:

```text
/sdcard/04_sys_ai_img/remote.jpg
```

Bagus untuk “tampilkan gambar ini sekarang”.

### Versi lanjut

Tambah arg optional:

```json
{
  "url": "...",
  "save_to_gallery": true,
  "display": true,
  "filename": "optional.jpg"
}
```

Jika `save_to_gallery` true, simpan ke:

```text
/sdcard/05_user_ai_img/<filename>
```

Lalu refresh SD image list agar masuk slideshow.

## Render design

Tambah event bit baru untuk remote image, misalnya bit 4:

```text
epaper_groups bit 4 -> render remote image path
```

Jangan langsung render dari MCP callback. Ikuti pola existing:

- MCP callback set state/event.
- `gui_user_Task()` mengambil mutex e-paper.
- Render dilakukan di task e-paper.
- Setelah selesai, mutex dilepas.

## Download implementation strategy

### Cepat tapi kurang ideal

Download full response ke PSRAM, lalu write ke SD.

Risiko:

- file S3 bisa lebih besar dari buffer.
- memory pressure.

### Lebih benar

Streaming HTTP ke SD card:

```text
HTTP GET
  while read chunk:
    write chunk to SD
```

Ini menghindari buffer besar dan lebih cocok untuk image dari S3/CloudFront.

## Format gambar

Backend sebaiknya menyiapkan image yang sudah cocok untuk e-paper:

- JPG baseline / PNG / BMP sesuai decoder.
- Aspect ratio dekat 800x480.
- File size wajar.
- Kontras dan palette cocok untuk e-paper.

MVP bisa download apa adanya, tetapi reliability lebih baik jika backend melakukan resize/convert dulu.

## Rencana implementasi bertahap

### Phase 1

- Tambah `remote_img_path` global.
- Tambah event bit render remote image.
- Tambah MCP `self.disp.open_image_url(url)`.
- Download ke `/sdcard/04_sys_ai_img/remote.jpg`.
- Render langsung setelah download.

### Phase 2

- Streaming download chunk-to-SD.
- Add `display` dan `save_to_gallery` args.
- Refresh SD card image list jika disimpan ke gallery.
- Improve error reporting.

### Phase 3

- Backend S3 presign service.
- Optional CloudFront support.
- Optional backend image resize/convert.
- Optional cache prompt/image metadata.

## Naming rekomendasi

Nama file/dokumen ini sengaja dibuat explicit:

```text
docs/PhotoPainter_AI_MCP_S3_Analysis.md
```

Nama branch:

```text
docs/mcp-s3-ai-analysis
```

# alc-gw-p4

M5Stack Unit PoE-P4 (ESP32-P4) 向け、C212 カメラ映像を RTSP pull → 拠点カメラ用シグナリング Durable Object 経由の P2P WebRTC で中継するファームウェア。

[alc-gw](https://github.com/ippoan/alc-gw) の中継実装 (Go/pion) の "P4 版" にあたる。管理者向け遠隔点呼の全景配信で、alc-gw が Windows GW 拠点向け、こちらは PoE-P4 (Windowsレス) 拠点向けの中継経路を担う想定。

当初は WHIP (RFC 9725) + クラウド SFU 方式だったが、同時複数視聴が不要なこと・[ippoan/alc-app](https://github.com/ippoan/alc-app) の cf-alc-signaling (Durable Objects) が既にあることから、「本ファームが signaling room へ常時接続し、admin (管理者ブラウザ) が現れた時だけ SDP を交換して P2P を開通させる」方式に転換した ([ippoan/alc-app#129](https://github.com/ippoan/alc-app/issues/129))。

## 現状

- Unit PoE-P4 実機での書き込み・動作確認まで完了 ([docs/hardware-notes.md](docs/hardware-notes.md))
- RTSP 側 (`main/`, [alc-gw-p4#1](https://github.com/ippoan/alc-gw-p4/issues/1)): 実機 (Tapo C212) で RTSP Digest 認証 (DESCRIBE 401→再送→200) から SETUP/PLAY までの通信を確認済み。自前 RTSP クライアント (`components/rtsp_client`) が `esp_media_protocols` (Digest 認証未対応) を置き換えている
- signaling 側: WHIP から DO シグナリング + P2P への転換を実装中 ([ippoan/alc-app#129](https://github.com/ippoan/alc-app/issues/129))。実SFU/実signaling接続時の動作確認は未検証 (`CONFIG_RELAY_SIGNALING_URL` は実運用時に設定が必要)

## main/ (中継本体)

C212 の stream2 (RTSP) を pull し、無トランスコードで admin へ P2P WebRTC 配信する。設定は `idf.py menuconfig` の "alc-gw-p4 Relay Configuration" (RTSP URL / signaling room の WebSocket URL / トークン、v1 はビルド時埋め込み)。

```powershell
$env:IDF_TOOLS_PATH = "C:\t\.embuild"
& "C:\t\.embuild\esp-idf\v5.5.3\export.ps1"
idf.py set-target esp32p4
idf.py menuconfig   # RTSP URL / signaling URL / トークンを設定
idf.py -p COM9 build flash monitor
```

## examples/hello_world

ESP-IDF 標準サンプルを、手元の実機 (rev v1.3) 向けに `sdkconfig.defaults` を調整したもの。実機の書き込み疎通確認に使用。

```powershell
$env:IDF_TOOLS_PATH = "C:\t\.embuild"
cd examples/hello_world
& "C:\t\.embuild\esp-idf\v5.5.3\export.ps1"
idf.py set-target esp32p4
idf.py -p COM9 build flash monitor
```

詳細は [docs/hardware-notes.md](docs/hardware-notes.md) を参照。

# alc-gw-p4

M5Stack Unit PoE-P4 (ESP32-P4) 向け、C212 カメラ映像を RTSP pull → WHIP publish で中継するファームウェア。

[alc-gw](https://github.com/ippoan/alc-gw) の WHIP publish 実装 (Go/pion) の "P4 版" にあたる。管理者向け遠隔点呼の全景配信で、alc-gw が Windows GW 拠点向け、こちらは PoE-P4 (Windowsレス) 拠点向けの中継経路を担う想定。

## 現状

- Unit PoE-P4 実機での書き込み・動作確認まで完了 ([docs/hardware-notes.md](docs/hardware-notes.md))
- 中継本体 (`main/`, [alc-gw-p4#1](https://github.com/ippoan/alc-gw-p4/issues/1)): 実機 (Tapo C212) で RTSP Digest 認証 (DESCRIBE 401→再送→200) から SETUP/PLAY までの通信を確認済み。自前 RTSP クライアント (`components/rtsp_client`) が `esp_media_protocols` (Digest 認証未対応) を置き換えている。WHIP publish 部分は SFU 未接続のため未検証 (`CONFIG_RELAY_WHIP_URL` は実運用時に設定が必要)。

## main/ (中継本体)

C212 の stream2 (RTSP) を pull し、無トランスコードで WHIP publish する。設定は `idf.py menuconfig` の "alc-gw-p4 Relay Configuration" (RTSP URL / WHIP URL / トークン、v1 はビルド時埋め込み)。規約は [ippoan/alc-gw の docs/whip-convention.md](https://github.com/ippoan/alc-gw/blob/main/docs/whip-convention.md) と共通。

```powershell
$env:IDF_TOOLS_PATH = "C:\t\.embuild"
& "C:\t\.embuild\esp-idf\v5.5.3\export.ps1"
idf.py set-target esp32p4
idf.py menuconfig   # RTSP URL / WHIP URL / トークンを設定
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

# Unit PoE-P4 ハードウェアメモ

M5Stack Unit PoE-P4 (ESP32-P4, 802.3at PoE, IP101GRI Ethernet PHY) を実機検証した際の知見。

## チップリビジョン (重要)

手元の実機は **ESP32-P4 revision v1.3** (efuse block revision v0.3) だった。ESP-IDF v5.5.3 のデフォルト `sdkconfig` は `CONFIG_ESP32P4_REV_MIN_301` (v3.1 以降のみ) を要求しており、そのままでは

```
A fatal error occurred: bootloader/bootloader.bin requires chip revision in range [v3.1 - v3.99] (this chip is revision v1.3). Use --force to flash anyway.
```

で書き込みが拒否される。

ESP32-P4 は **v3.0 未満と v3.0 以上でハードウェアが大きく異なり相互排他的**([Kconfig.hw_support](https://github.com/espressif/esp-idf/blob/v5.5.3/components/esp_hw_support/port/esp32p4/Kconfig.hw_support) のコメントより)。v1.x 実機向けにビルドするには以下が必要 (`examples/hello_world/sdkconfig.defaults` 参照):

```
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y   # これを先に有効にしないと REV_MIN_1 等の選択肢が出ない
CONFIG_ESP32P4_REV_MIN_1=y             # Rev v0.1 以上
```

M5Stack 公式の [M5Unit-PoE-P4-UserDemo](https://github.com/m5stack/M5Unit-PoE-P4-UserDemo) も同じ `REV_MIN_1` 設定でビルドされている。`--force` でリビジョンチェックを無視する方法もあるが、`P4_REV3_MSPI_CRASH_AFTER_POWER_UP_WORKAROUND` (v3.1 で修正された既知の MSPI クラッシュ対策) が無効なまま起動することになるため非推奨。上記設定の hello_world を実機に書き込み、リブートを跨いだ安定動作を確認済み (2026-07-19)。

## USB-C ポートの電源設計

Unit PoE-P4 の USB-C ポートは Host 用・OTG/デバッグ用の2つとも回路的に**双方向 (OTG) 設計**([Mainboard/Power Board Schematics](https://docs.m5stack.com/en/unit/Unit_PoE-P4) 解析より)。VBUS は `SYS_5V` とダイオード+ロードスイッチ (AW32901FCR) で OR 合流しており、GPIO で直接 ON/OFF できる経路は基板上に存在しない。CC 線のネゴシエーション次第では相手機器がホスト化し、逆に Unit PoE-P4 へ給電してしまう構造的リスクもある。USB 給電を能動制御したい用途(例: 周辺機器の電源スイッチング)には外付け回路が必要。

## 開発環境

- `IDF_TOOLS_PATH` はチップ非依存で共有できるが、C言語ビルド (`idf.py`) に必要な gdb/openocd/idf-exe/ccache/dfu-util 等は `idf_tools.py install --targets=esp32p4` で別途インストールが必要
- **Windows で Git Bash (MSys) から `export.sh` を実行すると `MSys/Mingw is not supported` で失敗する** — PowerShell で `export.ps1` を使うこと
- ビルドディレクトリのパスは短くすること。長いパスだと Windows の `CMAKE_OBJECT_PATH_MAX` (250文字) を超えてコンパイルが失敗する
- 実機は USB-C (OTG/デバッグ用ポート) 接続で `USB Serial/JTAG` として認識される。側面のリセットボタン2秒長押し→緑LED点灯でダウンロードモードに入れるが、通常の書き込みでは不要だった

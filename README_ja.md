# 📡 JJY-SIM R4（R3の設計資料も収録）

電波時計の補正信号をESP32-C3/ESP8684で自作！  
NTPから取得した正確な時刻をもとに、40kHz / 60kHz のJJY信号をPWM出力で模倣する、  
ミニJJYシミュレーターです。  
R2 / R3 / R4 は ESP-IDFで開発した共通ファームウェアを使用します。

**最新モデルはR4です。R3は旧モデルの設計資料として、参照用に残しています。**

ファームウェアのバージョンは **1.2.0** です。

👉 [English README is available here](README.md)

---

## R4 / R3 / R2 の構成

| 項目 | R4（最新モデル） | R3（旧モデル・参照用） | R2（旧モデル） |
|---|---|---|---|
| 外観 | アンテナをケース内部に配置 | アンテナがケース外部に配置 | R3と同様 |
| MCU | ESP32-C3 | ESP8684（ESP32-C2） | ESP32-C3 |
| 共振回路の切り替え | GPIO3で自動切り替え | 基板上のスイッチ | ハンダジャンパー |
| ファームウェア | 共通ソース、esp32c3向けビルド | 共通ソース、esp32c2向けビルド | 共通ソース、esp32c3向けビルド |
| ハードウェア資料 | 収録（r4 / r4.1, r4_ant） | 参照用として収録（r3）| 未収録 |

R3とR4はPCBアンテナを使用しています。R4はメインPCBとアンテナPCB 3枚のスタック構成で、アンテナをケース内に収めています。R3はメイン回路とアンテナが一体のPCBで、アンテナがケース外部に出る構成です。

R2の外観はR3と同様ですが、MCUはESP32-C3で、共振回路はハンダジャンパーで切り替えます。共通FWのC3向けビルドを使用できます。R2のハードウェア資料は、このリポジトリには収録していません。

R2／R3／R4の共通ファームウェアソースは `firmware/idf/jjy-sim` にあります。
R4はeFuseの `USER_DATA_JJYSIM_HW_GENERATION` が `4` の場合に識別されます。未設定のESP32-C3はR2として動作します。自作R4基板では、この識別情報の設定が必要です。eFuseへの書き込みは不可逆なので、対象基板と `main/esp_efuse_custom_table.csv` を確認してから行ってください。

USBでの書き込み方法は、[書き込み手順](firmware/idf/jjy-sim/README.md)を参照してください。

設計データは `hardware/KiCad/r3`、`r4`、`r4.1`、`r4_ant`、共通ライブラリは `hardware/KiCad/lib` にあります。各基板のガーバーはそれぞれの `PLOT` に、ケースは `hardware/case_3d/r3` と `r4` にあります。

`hardware/KiCad/r4.1` は、R4のPCB上の部品レイアウトを変更した設計データです。レイアウト変更に伴い、共振回路のL/C/Rの並び順も変更しています。

## 🪛 特徴

* **NTP**から取得した時刻で、**JJY 40kHz / 60kHz**の信号を生成
* **PCBアンテナ**を搭載（R4はケース内蔵）
* **OLED**に時刻・動作状態を表示
* **ブラウザ**からWi-Fiや各種動作を設定
* **毎時モード・ナイトモード・スクリーンセーバー**に対応
* **NTPサーバー**として、LAN内の機器へ時刻を配信
* **OTAアップデート**で、Wi-Fi経由のファームウェア更新に対応

*R4の外観*  

<a href="./images/jjy-sim-r4.jpg">
<img src="./images/jjy-sim-r4.jpg" alt="JJYシミュレーター Rev.4" width=400 style="max-width:100%; height:auto;"></a><br/>

*参考：下は旧モデルR3の外観*  

<a href="./images/jjy-sim-r3.jpg">
<img src="./images/jjy-sim-r3.jpg" alt="JJYシミュレーター Rev.3" width=300 style="max-width:100%; height:auto;"></a><br/>

---

## 🛠 ビルドと書込み (ESP-IDF / CLI)

ESP-IDF **v5.5.4** の環境で、`firmware/idf/jjy-sim` に移動して実行します。Arduino環境ではビルドできません。

R3（ESP8684 / ESP32-C2）:

```bash
idf.py -B build-c2 -D IDF_TARGET=esp32c2 -D SDKCONFIG=sdkconfig.c2 build
idf.py -B build-c2 -D IDF_TARGET=esp32c2 -D SDKCONFIG=sdkconfig.c2 -p COMx flash monitor
```

R4（ESP32-C3）:

```bash
idf.py -B build -D IDF_TARGET=esp32c3 -D SDKCONFIG=sdkconfig.c3 build
idf.py -B build -D IDF_TARGET=esp32c3 -D SDKCONFIG=sdkconfig.c3 -p COMx flash monitor
```

`COMx` は接続先に合わせて変更してください。OTA用パーティション構成は `partitions.csv` に含まれています。旧版からの移行では、アプリだけでなくパーティションテーブルを含むUSB書き込みが必要です。

### R4の識別情報をeFuseに書き込む

自作R4基板のESP32-C3には、R4として識別するための値 `4` を書き込みます。**R3には実行しないでください。eFuseへの書き込みは不可逆です。**

ESP-IDFの環境で `firmware/idf/jjy-sim` に移動し、`COMx` を対象R4のポートに置き換えて実行してください。以下はUSB接続時のコマンドです。

まず現在の値を確認します。

```powershell
espefuse --chip esp32c3 -p COMx --before usb_reset --extend-efuse-table main/esp_efuse_custom_table.csv summary
```

`USER_DATA_JJYSIM_HW_GENERATION` が `0`（未設定）であることを確認してから、次のコマンドで `4` を書き込みます。すでに `4` なら書き込みは不要です。その他の値の場合は、この手順で上書きしないでください。

```powershell
espefuse --chip esp32c3 -p COMx --before usb_reset --extend-efuse-table main/esp_efuse_custom_table.csv burn_efuse USER_DATA_JJYSIM_HW_GENERATION 4
```

ツールが表示する対象と変更内容を確認し、確認プロンプトに従って書き込みます。完了後、上の `summary` コマンドを再実行し、値が `4` になったことを確認してください。基板を再起動すると、ファームウェアがR4として識別します。

---
## 🛠 ビルドと書込み (VSCode / GUI)

VSCode + ESP-IDF Extension を使用した開発も可能です。

### ■ 推奨拡張機能

* ESP-IDF Extension (Espressif)

VSCodeに拡張機能をインストールすることで、

* ビルド
* 書き込み
* シリアルモニタ

をGUIから操作できます。

### ■ セットアップ

ESP-IDF Extensionのセットアップに従って環境を構築してください。

拡張機能がインストールされていれば、

* `Build`
* `Flash`
* `Monitor`

などがワンクリックで実行できます。

### ■ ワークスペース  


VSCodeで開く場合は、ソースコードフォルダ内の  
**`JJY-SIM.code-workspace`** を開くことで、  
このPCのESP-IDF 5.5.4環境と `C3 (R2 / R4)` または `C2 (R3)` の構成を選択して作業できます。

---

## ⚙️ 使用方法

世代ごとの設定方法・画面・操作は、[JJY-SIM サポートページ](https://blog.shachi-lab.com/support/)の各マニュアルを参照してください。
R4の共振回路はWeb画面のBAND設定に連動します。R3はBAND設定に加えて基板上のスイッチも合わせてください。

---
## 🔌 ピンアサイン（主なもの）

| 機能 | GPIO | 備考 |
|------|------|------|
| JJY PWM出力 | 10,4 | 変調された信号で、Hブリッジ用にA/Bの反転出力 |
| R4 BAND切り替え | 3 | R4のみ。共振回路を切り替え |
| CONFIGスイッチ | 9 | Wi-Fi設定用（**BOOTピンと兼用**） |
| ACT LED | 5 | 動作中LED |
| IND LED | 0 | 状態表示用LED（共用可） |
| OLED Reset | 2 | OLEDモジュールリセット |
| OLED SDA|7| OLED I2C データー|
| OLED SCL |6|OLED I2C クロック|

---

## 🧾 ファイル構成

### 💻 ファームウェア

* ファームウェアのソースコードは、**`firmware/idf`** フォルダにあります。
* R2 で使用していた Arduino用のOLED ドライバライブラリを、自身でIDFのコンポーネントとしてポーティングして再構築したライブラリを使用しています。  
なお、このコンポーネントは、GitHubで公開しています。  
👉 [SSD1306 OLED display driver](https://github.com/Shachi-lab/oled_display)
* `storage_key.h` にはダミーの値が設定されていますがそのまま使用できます。  
  セキュリティを強化するため、ビルド前にこれらの値を独自の値に置き換えることをお勧めします。

### 🧰 ハードウェア

* ハードウェア設計データは、**`hardware`** フォルダ にあります。
* 回路図、基板設計データーは、**`hardware/KiCad`** フォルダにあります。 
  * **KiCad Ver.10** に対応しています（Ver.9以前では開けません）  
  * **ガーバーデータ** は **`hardware/KiCad/<revision>/PLOT`** フォルダに含まれており、そのまま基板製作が可能です。
* ケースの3Dデーターは、**`hardware/case_3d`** にあります。

---

## 🔧 自作・組み立て

### R4のアンテナ

R4を自作する場合は、**アンテナPCB 3枚をスタックする組み立て作業が必要**です。メインPCBと合わせてケース内部に収めます。

基板厚は、**メインPCBが1.6 mm、アンテナPCBが0.8 mm**です。

### OLEDの台座（R3 / R4共通）

R3 / R4ともに、PCB上にOLEDの台座部分をタブ形状で設けています。**組み立て時にこの部分を折り取り、OLEDの台座として使用してください。**

### ケース固定用ネジ

**六角 M2 × 5 mm タッピングねじ**を使用します。

---

## 📸 外観・実装例

以下の写真はR4の基板とケースです。  
黒基板＋黒ケースでまとめて、表示だけが浮いて見えるようにしています。

<a href="./images/jjy-sim-r4_pcb.jpg">
<img src="./images/jjy-sim-r4_pcb.jpg" alt="JJYシミュレーター Rev.4 PCB" width=300 style="max-width:100%; height:auto;"></a>
<a href="./images/jjy-sim-r4_case.jpg">
<img src="./images/jjy-sim-r4_case.jpg" alt="JJYシミュレーター Rev.4 ケース付" width=300 style="max-width:100%; height:auto;"></a><br/>

### R3・参考

以下の写真はR3の基板とケースです。  

<a href="./images/jjy-sim-r3_pcb.jpg">
<img src="./images/jjy-sim-r3_pcb.jpg" alt="JJYシミュレーター Rev.3 PCB" width=200 style="max-width:100%; height:auto;"></a>
<a href="./images/jjy-sim-r3_case.jpg">
<img src="./images/jjy-sim-r3_case.jpg" alt="JJYシミュレーター Rev.3 ケース付" width=200 style="max-width:100%; height:auto;"></a><br/>

---

## ⚠️ ご注意

R4は動作中にアンテナ部分が多少発熱します。

思った以上に電波が飛びます。  
電波法を守って実験してください。

---

## 🔗 サポート / ドキュメント  

最新版の情報、設定方法、詳しい使い方については、以下のサポートページをご覧ください。  
👉 [JJY-SIM サポートページ](https://blog.shachi-lab.com/support/)

---

## 📝 クレジット表記について

JJY-SIM は MIT License で公開しています。  
自由に使ったり、改造したり、自分の作品のベースにしてもらってかまいません。  

JJY-SIM をベースにしたものを販売・配布・公開する場合は、製品ページ、README、説明書、パッケージなど、どこか見える場所に **Shachi-lab / JJY-SIM** のクレジット表記を入れてください。

表記例：

* Designed by Shachi-lab
* Based on JJY-SIM by Shachi-lab
* Inspired by JJY-SIM by Shachi-lab

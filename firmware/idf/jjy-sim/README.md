# JJY-SIM ファームウェアソース

ESP-IDF **5.5.4** 用のソースです。R2／R4はESP32-C3、R3（ESP8684）はESP32-C2を使用します。
配信ファイル生成・USBアップデーター・製造用スクリプトは含みません。

## VS Code

1. EspressifのESP-IDF拡張とESP-IDF 5.5.4をインストールします。
2. このフォルダーの `JJY-SIM.code-workspace` を開きます。
3. `ESP-IDF: Select Current ESP-IDF Version` で、このPCの5.5.4環境を選択します。
4. `ESP-IDF: Select Project Configuration` で `C3 (R2 / R4)` または `C2 (R3)` を選択します。
5. `Build` を実行します。
6. USBで対象機器を接続し、接続先のシリアルポートとUART書き込みを選択して `Flash` を実行します。
7. 必要に応じて `Monitor` でログを確認します。

SDKやコンパイラ、シリアルポートは各PCで選択してください。プロジェクト内のパスは相対指定です。
コンテナを使用する必要はありません。C2とC3は順番にビルドしてください。

## ESP-IDF コマンドライン

ESP-IDF 5.5.4の環境を有効にしたターミナルで、このフォルダーに移動して実行します。

R2／R4（ESP32-C3）：

```sh
idf.py -B build -D IDF_TARGET=esp32c3 -D SDKCONFIG=sdkconfig.c3 build
idf.py -B build -D IDF_TARGET=esp32c3 -D SDKCONFIG=sdkconfig.c3 -p COMx flash monitor
```

R3（ESP8684 / ESP32-C2）：

```sh
idf.py -B build-c2 -D IDF_TARGET=esp32c2 -D SDKCONFIG=sdkconfig.c2 build
idf.py -B build-c2 -D IDF_TARGET=esp32c2 -D SDKCONFIG=sdkconfig.c2 -p COMx flash monitor
```

`COMx` は対象機器の接続ポートに置き換えます。Linuxでは `/dev/ttyUSB0`、macOSでは `/dev/cu.usbserial-…` など、実際に認識されたポートを指定してください。
モニターは `Ctrl+]` で終了できます。

`flash` はブートローダー・パーティションテーブル・アプリを含めてUSB書き込みします。
OTA非対応の旧パーティションから移行するときも、この手順を使用してください。
R4の自作基板では、リポジトリの [日本語README](../../../README_ja.md) に記載したeFuse識別情報の設定も確認してください。

## 設定とビルド生成物

`sdkconfig.c3`／`sdkconfig.c2` と `CMakePresets.json` でターゲット別の設定を管理します。
初回ビルド時、ESP-IDF Component Managerが `main/idf_component.yml` と `dependencies.lock` に従ってOLED表示部品を取得するため、インターネット接続が必要です。
`build`／`build-c2`／`managed_components` は各PCで生成します。ビルド後にフォルダーを移動・改名する場合は、ビルドフォルダーを生成し直してください。

## 公開ソースの保存用キー

`main/resource/storage_key.h` は公開用ダミーキーです。製品用の実キーは含みません。
このソースからビルドしたFWでは、製品用キーで保存された設定を復号できないため、Wi-Fiなどの設定を再入力してください。
OTA更新機能の実装は含みますが、配信サーバー用のツールや配信用ファイルは含みません。

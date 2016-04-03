# BonDriver_Popen

[BonDriverProxy_Linux](https://github.com/u-n-k-n-o-w-n/BonDriverProxy_Linux)
で動作する Linux 用の BonDriver です。

目的は Linux 環境で録画コマンドは存在するが BonDriver が存在しないチューナーを取り敢えず
BonDriverProxy_Linux で使えるようにしようということです。

BonDriver_Popen の主な動作は以下になります。

* OpenTuner に対しては何もせずに TRUE を返します
* SetChannel 時に fork, exec して録画コマンドを子プロセスで実行します
* pipe から録画コマンドの出力を read します
* CloseTuner 時に SIGTERM を送信して子プロセスを終了します

例えば、チューナーが KTV-FSUSB2N の場合、録画コマンドは recfsusb2n のことを指し、
recfsusb2n を実行して TS を取得し、KTV-FSUSB2N の BonDriver の様に振る舞います。

チャンネル設定にはプロセスの停止と起動を伴うのである程度の時間がかかります。
必要に応じてマージンを多めに設定する等の対処をして下さい。
もちろん、BonDriver_Popen 単体では動作しません。

## インストール

ビルドには cmake が必要です。
BonDriver の配置場所は特に決まっていませんが [recbond] (https://github.com/dogeel/recbond)
のデフォルトである `/usr/local/lib/BonDriver` に配置するのが良いでしょう。

```
git clone https://github.com/hendecarows/BonDriver_Popen.git
cd BonDriver_Popen/build/
cmake ..
make
sudo mkdir -p /usr/local/lib/BonDriver
sudo cp BonDriver_Popen.so /usr/local/lib/BonDriver/BonDriver_Popen-T0.so
sudo cp BonDriver_Popen.so /usr/local/lib/BonDriver/BonDriver_Popen-S0.so
sudo cp ../src/BonDriver_Popen.so.conf /usr/local/lib/BonDriver/BonDriver_Popen-T0.so.conf
sudo cp ../src/BonDriver_Popen.so.conf /usr/local/lib/BonDriver/BonDriver_Popen-S0.so.conf
sudo vi /usr/local/lib/BonDriver/BonDriver_Popen-T0.so.conf
sudo vi /usr/local/lib/BonDriver/BonDriver_Popen-S0.so.conf
```

アップデートは以下の手順になります。
```
cd BonDriver_Popen
git fetch
git pull
cd build/
cmake ..
make
sudo cp BonDriver_Popen.so /usr/local/lib/BonDriver/BonDriver_Popen-T0.so
sudo cp BonDriver_Popen.so /usr/local/lib/BonDriver/BonDriver_Popen-S0.so
sudo vi /usr/local/lib/BonDriver/BonDriver_Popen-T0.so.conf
sudo vi /usr/local/lib/BonDriver/BonDriver_Popen-S0.so.conf
sudo systemctl restart BonDriverProxy.service
```

BonDriver (BonDriver_Popen.so) や 設定ファイル (BonDriver_Popen.so.conf)
を変更した場合は BonDriverProxy(Ex) を再起動して下さい。BonDriverProxy(Ex)
は一旦ロードした BonDriver は使用クライアントがいなくなっても dlclose しません。
設定ファイルは BonDriver のロード時に読み込みますので BonDriverProxy(Ex) が
BonDriver をロード済みの場合は変更が反映されません。再起動手順は
BonDriverProxy(Ex) の起動方法によって異なりますので環境に合わせて変更して下さい。

## 設定ファイル

設定ファイルは BonDriver 本体の BonDriver_Popen.so と同じディレクトリに 
BonDriver_Popen.so.conf として配置します。.conf ではなく .so.conf ですので注意して下さい。

```
; ISDB-S=0,ISDB-T=1 のどちらかを指定する
#ISDBTYPE=0

; ISDB-S のデフォルト実行コマンドを指定する
; {channel}をコマンド用チャンネル番号で置換した上で実行する
; コマンドはデータを標準出力へ出力する設定とする
#ISDBSCOMMAND=pxbcud.py -c {channel}

; ISDB-T のデフォルト実行コマンドを指定する
#ISDBTCOMMAND=recfsusb2n {channel} - -

; GetSignalLevelで返す値
; 受信状態に関わらずこの値を返す
#SIGNALLEVEL=20

; 名称, BonDriverとしてのチャンネル番号, コマンド用チャンネル番号, サービスID, 個別実行コマンド
#ISDB_S
; BS
BS朝日   0   151   151
BS-TBS   1   161   161   recpt1 {channel} - -
```
まず、チューナーの種類に応じて `#ISDBTYPE` を適切に設定して下さい。
付属の設定ファイルは ISDB-T (地上波) 用の `#ISDBTYPE=1` になっていますので
ISDB-S (BS/CS) の場合は `#ISDBTYPE=0` に変更して下さい。

次に `#ISDBSCOMMAND` と `#ISDBTCOMMAND` で それぞれ ISDB-S と ISDB-T
用のコマンドを指定します。コマンドは TS を STDOUT に出力する形式で指定します。
チャンネル設定で個別実行コマンドが設定されていない場合はコマンド内の文字列 `{channel}`
をコマンド用チャンネル番号で置換したものが実行コマンドになります。
BS朝日の場合は以下になります。

```
pxbcud.py -c 151
```

一方、BS-TBSのように個別実行コマンドが指定されている場合は同様に `{channel}`
を置換してそのコマンドを実行します。

```
recpt1 161 - -
```

GetSignalLevelで返る値は受信状態に関係なく `#SIGNALLEVEL` で指定した一定値になります。
その他の詳細な内容はサンプル BonDriver_Popen.so.conf 内のコメントを確認して下さい。

## ライセンス

* MIT ライセンス

BonDriver_Popen は BonDriverProxy_Linux の BonDriver_DVB 関係のソースをほぼそのまま利用しています。
従ってオリジナルと同じ MIT ライセンスです。

BonDriverProxy_Linuxのライセンスは以下を確認して下さい。

* https://github.com/u-n-k-n-o-w-n/BonDriverProxy_Linux


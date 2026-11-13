# TvRockOnTVTest

TVTest と TvRockDTV.dll を接続する Windows 用 C++ プラグインです。TVTest の TS（Transport Stream）を DLL へ転送し、TvRockGO 側からの選局・録画・終了要求を TVTest に伝えます。x64 / x86 の両方に対応しています。

このリポジトリには、TVTest 用プラグインのソース・ビルドスクリプト・回帰テストと、TSTask / RecTest へ組み込むための差し替えソースを含みます。TVTest、BonDriver、TvRockDTV.dll、TvRockGO 本体は別途用意してください。

## 導入

1. TVTest と同じアーキテクチャのプラグインを、TVTest の `Plugins` フォルダへ配置します。
   - x64: `TvRockOnTVTest.x64.tvtp`
   - x86: `TvRockOnTVTest.tvtp`
2. 同じアーキテクチャの `TvRockDTV.dll` を、プラグインの隣または `TVTest.exe` の隣へ配置します。`tvrockdtv` プロジェクトでビルドした DLL を利用できます。
3. TvRockGO のチューナー設定に合わせて、TVTest の起動オプションに `/DID A` などのデバイス識別子を指定します。

```text
TVTest.exe /d BonDriver.dll /DID A
```

`BonDriver.dll` は使用するドライバ名に置き換えてください。`/DID` は英字 1 文字（A～Z、大文字・小文字とも可）を受け付けます。未指定または不正な指定では DLL との連携を開始しません。起動済みの TVTest に別の DID の実行要求が届くと連携を再初期化し、DID を含まない `/d` 指定の実行要求では連携を終了します。

DLL は次の順序で検索します。作業ディレクトリには依存しません。

| 順序 | 検索先 | x64 のファイル名 | x86 のファイル名 |
| --- | --- | --- | --- |
| 1 | プラグインのフォルダ | `TvRockDTV.x64.dll` → `TvRockDTV.dll` | `TvRockDTV.dll` |
| 2 | `TVTest.exe` のフォルダ | `TvRockDTV.x64.dll` → `TvRockDTV.dll` | `TvRockDTV.dll` |

x64 と x86 の DLL を同じフォルダへ置く場合は、x64 側を `TvRockDTV.x64.dll` に改名してください。ビルドスクリプトによる自動配置は行いません。

## 設定

設定ファイルは、プラグインと同じフォルダに置く、拡張子を `.ini` に変更したファイルです。

- x64: `TvRockOnTVTest.x64.ini`
- x86: `TvRockOnTVTest.ini`

既存の設定ファイルを引き続き使用できます。読み取りには Windows の INI API を使用するため、ソースの文字コードに合わせて既存の INI を UTF-8 に変換する必要はありません。

| セクション | キー | 内容 |
| --- | --- | --- |
| `[Settings]` | `BufferLength` | TS バッファのパケット数。初期値は 20000、範囲は 10000～100000 |
| `[DID_A]` など | `TerrestrialIndex` | 地上波のチューニング空間インデックス |
| `[DID_A]` など | `TerrestrialIndex2` | 2 つ目の地上波のインデックス。`TerrestrialIndex` を指定した場合に読み込み |
| `[DID_A]` など | `BSIndex` / `CSIndex` | BS / CS のチューニング空間インデックス |

DID 別のインデックスは 0 から始まります。未指定の場合はチャンネル情報などから検索し、ドライバ変更時には検索し直します。BS/CS は対象のチューニング空間からサービスIDでチャンネルを検索します。固定のTSID・周波数対応表と `[BSFrequency]` / `[CSFrequency]` の読み込みは廃止しました。既存の該当セクションは無視します。

## 動作

TVTest 用プラグインは、TS をバッファ経由で専用スレッドから DLL へ転送します。データがない間はイベントを待機し、選局時には処理中の TS 転送を終えてからバッファを破棄してストリームを切り替えます。

DLL の別スレッドから届く録画・選局などの要求と、TVTest のステータス取得は、TVTest のメインスレッドで処理します。ビットレートは TVTest が返す bits/s の値を使用します。終了時には待機中の要求を解除し、TS スレッドの終了を待ってから DLL を解放します。初期化に失敗した場合も、確保済みのバッファ・イベント・スレッドなどを解放します。

`TvRockDTV.h` は DLL との ABI 互換性を保つため、仮想関数の型と宣言順を維持しています。クラス名の比較には文字列比較と NULL 入力の検証を使用し、ログの書式化にはバッファ長を指定します。

地上波・BSのサブチャンネル選局は、親局のチャンネルを検索し、保存したサブチャンネルのサービスIDを選局APIへ渡します。サブチャンネル指定は次の選局要求で一度だけ消費し、選局失敗時も後続の要求へ持ち越しません。

BS/CS の選局通知では、`Channel` の上位16bitに放送種別（BS=2、CS=3）、下位16bitにTSID（不明なら0）を入れます。地上波では上位16bitに放送種別1、下位16bitに従来のチャンネル値を入れます。フルセグ・ワンセグの区別はTvRockGOがSDT等から登録した局情報のSIDで判定します。DLLはこの値をそのまま転送します。

## ビルド

Visual Studio または Visual Studio Build Tools の「C++ によるデスクトップ開発」と Windows SDK が必要です。ソース・ヘッダは UTF-8 BOM 付きで、コンパイラには `/utf-8` と C++17 を指定しています。Release は `/MD`、Debug は `/MDd` を使用します。

`build.ps1` は `vswhere` で Visual Studio を検出し、子プロセス内でビルド環境を設定します。既定の構成は x64 / Release です。

コマンドプロンプトから:

```bat
build.bat
build.bat -Architecture x86
build.bat -Architecture x64 -Debug
```

PowerShell から:

```powershell
.\build.bat
.\build.ps1 -Architecture x64
.\build.ps1 -Architecture x86 -Debug
```

失敗時は非ゼロの終了コードを返します。`build.bat` をダブルクリックした場合も x64 / Release をビルドします。

| 構成 | 成果物 |
| --- | --- |
| x64 Release | `target\x86_64-pc-windows-msvc\release\TvRockOnTVTest.x64.tvtp` |
| x86 Release | `target\i686-pc-windows-msvc\release\TvRockOnTVTest.tvtp` |
| Debug | 上記の `release` を `debug` に置き換えたフォルダ |

## TSTask / RecTest への組み込み

TSTask / RecTest は、プラグインを配置する方式ではなく、各アプリケーションのソースを差し替えて再ビルドします。

1. 対象アプリケーションの `TvRock.cpp` と `TvRock.h` を、このリポジトリの `TSTask/` または `RecTest/` にある同名ファイルで置き換えます。
2. 対象アプリケーションの `TvRockDTV.h` を、このリポジトリ直下の `TvRockDTV.h` で置き換えます。
3. 対象アプリケーションのビルド手順で再ビルドし、実行ファイルと同じアーキテクチャの DLL を実行ファイルの隣へ配置します。

リモートUDP送信処理はTvRock.cpp内で完結し、本体の標準UDP送信・設定・UIは変更しません。`GetFrequencyByTSID` は呼び出し口を残し、固定表を使わず0を返します。本体側の既存処理は周波数の補完を行わず継続します。

これらのフォルダには差し替え用ファイルのみを含みます。元のアプリケーションのソースとビルド環境が必要です。このリポジトリのビルドスクリプト・回帰テスト・CI の対象は TVTest 用プラグインです。

## 検証

PowerShell から、各アーキテクチャの回帰テストを実行できます。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -Architecture x64
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\test.ps1 -Architecture x86
```

テスト用 DLL と模擬 TVTest を `target\tests\x64` / `target\tests\x86` に生成し、TS 転送、初期化失敗時の後始末、再初期化、サービス指定、固定表にないBS/CSのTSID通知、TSID未取得時の放送種別、メインスレッドでの録画要求処理、ビットレート、終了時の要求解除、終了要求を検証します。

このリポジトリと同じ親フォルダに `tvrockdtv` プロジェクトがある場合は、次の追加検証も行います。

- `tvrockdtv\compat\TvRockDTV.h` があれば、そのヘッダでテスト DLL をコンパイルしてヘッダ間の ABI 互換性を確認します。なければ、このリポジトリのヘッダを使用します。
- `tvrockdtv\target\<ターゲット>\release\TvRockDTV.dll` があれば、実 DLL のロード・オブジェクト生成・破棄も確認します。

実 DLL の `Initialize` は呼び出さず、TvRockGO への接続や実録画は行いません。実際の TVTest・チューナー・ TvRockGO を使う視聴、予約録画、選局は別途確認してください。

## 配布と CI

両アーキテクチャの Release プラグインをまとめるには、次のコマンドを実行します。

```powershell
.\build-artifact.ps1
```

x64、x86 の順にビルドし、プラグインと README・ライセンス関連文書を次のフォルダへコピーします。

```text
tvrockontvtest/
  README.md
  LICENSE.md
  THIRD_PARTY_NOTICES.md
  licenses/
    CC0-1.0.txt
    GPL-2.0.txt
  x64/TvRockOnTVTest.x64.tvtp
  x86/TvRockOnTVTest.tvtp
```

GitHub Actions は、すべてのブランチへの push 時に x64 / x86 の回帰テストだけを実行します。

タグを push したときは GitHub Actions と GitLab CI が実行されます。GitHub Actions は回帰テストを再実行せず、GitLab CI は従来どおり回帰テストの成功後にプラグインをビルドします。ブランチ push でテスト済みのコミットにタグを付けてください。ビルドしたプラグインは、ジョブの成果物と、タグに対応する Release の `tvrockontvtest.zip` として配布します。既存の Release があれば、説明と他の添付ファイルを維持して同名の配布ファイルを更新します。

```sh
git tag v1.0.0
git push origin v1.0.0
```

GitHub Actions の定義は `.github/workflows/ci.yml` です。Release の登録には CI の `GITHUB_TOKEN` を使用します。

GitLab CI の定義は `.gitlab-ci.yml` で、`publish-gitlab-release.ps1` が `CI_JOB_TOKEN` を使って ZIP を Generic Package Registry に保存し、Release の Assets にリンクを登録します。Package Registry を有効にしてください。runner のタグには `saas-windows-medium-amd64` を指定しています。独自の runner を使用する場合はタグを変更し、PowerShell、Visual Studio C++ Build Tools、Windows SDK を用意してください。

## SRTリモート受信

BonDriver_SRTを使用する場合もTvRockDTVの受信通知とサービス選択に対応します。受信ドライバー名に `SRT` を含めてください。対応版RockbarはUDP用の `/port` に代わり `/chspace 0 /chi N` で受信チャンネルを指定します。BonDriver_SRT.iniのBasePortとRockbarの基準ポート・受信ポートを一致させてください。LAN用のListenAddress、遅延、パスフレーズもINIへ設定します。送信側TvRockDTV.dll、TvRockGO、RockbarはSRT対応版を使用します。

## リモートUDP視聴

RockRequest_V101にStreamStart(address,port)、StreamStop()、GetStreamStatus(status)を追加しています。V100のABIは維持します。兄弟TVTestはセッション専用の/pidと/parentで接続し、録画側とは独立した準備・受信状態を報告します。TvRockGOとexe版Rockbarも対応版へ更新してください。設定手順はTvRockGOのdocs/remote-viewing.mdを参照してください。

リモート視聴の128 TSパケット単位の集約（不足時は最大10ms待機）・4MiB送信バッファ・送信内容の選別は、各ホストのTvRock.cppに含まれます。送信PCで録画アプリを再ビルドし、exeを更新してください。

## サービス名のステータス表示

ステータス項目の追加に対応したTVTestでは、ステータスバー設定に「TvRock サービス名」が追加されます。有効にして表示位置を選ぶと、現在の受信サービスと番組名を「放送局：　番組名」と表示します。番組名を取得できない場合は番組名の部分を空欄にします。表示幅はTVTestのステータスバー設定で変更できます。未受信時は「サービス待ち」です。既存のUDPチャンネル表示は独立した項目です。TVTest本体の変更は不要です。

TVTestを配信元にする場合、対応版TvRockDTV.dllのRust配信モードをプラグインが有効にし、復号後のTSを直接渡します。送信内容と開始・停止はTvRockGO／Rockbarから操作します。TVTest本体の変更は不要です。`/parent`で接続する受信用TVTestでは配信モードが無効になり、受信TSを再送信しません。

## TSファイル再生対応

TvRockDTVからの要求を、既存のメインスレッド用ディスパッチャーで処理します。従来方式の一時停止時は最後の映像が残るようビューアーを維持し、字幕解析状態をリセットします。再開・シーク時はTVTestの`RESET_ALL`とTvRockDTVの`TvRockDTVPlaybackReset`を呼び、受信・字幕解析を再開できる状態にします。ファイル送出の停止とSRT再接続はTvRockGOが管理します。

更新したTvRockGO・Rockbar・TvRockDTVとの組み合わせで使用してください。TSの編集は行いません。受信済みデータによる停止の遅れ、映像復帰時間は実機確認が必要です。

### TSファイル再生の新旧互換制御

TVTest SDKを更新していますが、基本APIとChannelInfoなどの構造体は従来のSDK 0.0.10レイアウトを維持します。
フィルタグラフの初期化・終了イベント（SDK 0.0.14）は実行時に対応を問い合わせ、対応するTVTestだけで使用します。
`IMediaControl`の取得済み／未初期化を分けてTvRockDTVへ登録し、旧TvRockDTVで登録用exportがない場合も従来どおり動作します。
TvRockGOからの`pause-direct`／`resume-direct`では、既存のメインスレッド用ディスパッチャーでPause／Runを実行し、実際の状態を最大250ms確認します。
取得したCOM参照はグラフ終了時に解放します。グラフ未取得・未対応・操作失敗の場合は成功応答を返さず、TvRockGOの従来方式への切り替えに任せます。
シークと旧コマンドは従来のRESET_ALL／字幕リセット方式です。実映像・音声と再開位置の連続性はデコーダー／レンダラーごとの実機確認が必要です。

### TSファイル再生停止時の黒画面待受

対応するTvRockGO・TvRockDTVから`idle`が届くと、TVTestのメインスレッドで`RESET_ALL`と`SetPreview(false)`を実行します。TVTestは終了せず、映像表示を止めた待受になります。次の再生開始時の`reset`で、この処理が無効化したプレビューを`SetPreview(true)`に戻します。一時停止は最後の映像を保持します。対応確認は既存の予約コールバックを使い、グラフ制御の対応とは独立しているため古いTVTestでも既存のSetPreview APIが使用できれば対応します。

### 通信の優先度とBML

対応するTvRockDTV.dllはBML関連の転送を既定で停止します。BMLを使う場合は、TVTest・TSTask・RecTestの起動引数に`/bml`を追加してください。BMLの実装やプラグインのABIを削除する変更ではありません。字幕・EPG・ロゴ取得は継続します。対応TvRockGOからDSM-CCロゴの保存完了通知を受けると、DLLは同じモジュール・バージョンのDDB再送を停止し転送します。

## ライセンス

ファイルごとの利用条件は [LICENSE.md](LICENSE.md)、第三者コードの出典・改変内容は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。TVTest SDK は CC0、TSTask 由来のソースは GPL v2 表記に従います。プラグインの既存の Public Domain 表記だけをリポジトリ全体の利用条件として扱うことはできません。

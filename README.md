# BonDriverRelay

## 機能

Windows PCに接続したテレビチューナーを、BonDriver経由でネットワーク共有するソフトウェアです。TVTest・EDCBなどから `BonDriver_Relay.dll` を読み込んで利用します。

- 複数チューナーの割り当てと、同一チャンネルのTS共有。空きがない場合は既存の視聴・録画を維持して要求を拒否します。
- 外部の `tsukumijima/libaribb25` によるサーバー側B25復号。共有ストリームごとに処理します。
- コンソール起動とWindowsサービス運用、ローテーション付きログ。
- 簡素なWeb管理画面、JSON API、Prometheus形式のメトリクス。
- 複数サーバーからの接続先選択と、管理画面での各サーバーの状態表示。
- x64 / x86のサーバー・クライアント。x86のアプリからx64のサーバーにも接続できます。

## 導入方法

### 1. ダウンロードと準備

Windows 10 / 11のPCと、動作するテレビチューナー・対応BonDriverを用意してください。利用するアプリとチューナーのセットアップは先に済ませます。

[Releases](https://github.com/shirokuma1101/BonDriverRelay/releases/latest) からZIPをダウンロードします。GitHubの「Source code」は実行ファイルを含みません。

| ZIP | 選び方 |
|---|---|
| [BonDriverRelay-v0.1.0-win-x64.zip](https://github.com/shirokuma1101/BonDriverRelay/releases/download/v0.1.0/BonDriverRelay-v0.1.0-win-x64.zip) | 64bitのBonDriverを使うサーバー、64bit版TVTest・EDCB用 |
| [BonDriverRelay-v0.1.0-win-x86.zip](https://github.com/shirokuma1101/BonDriverRelay/releases/download/v0.1.0/BonDriverRelay-v0.1.0-win-x86.zip) | 32bitのBonDriverを使うサーバー、32bit版TVTest・EDCB用 |

サーバー用とアプリ用で必要なbit数が異なる場合は、両方ダウンロードしてください。

使用する各PCに、Microsoftの最新 [Visual C++ v14再頒布可能パッケージ](https://learn.microsoft.com/ja-jp/cpp/windows/latest-supported-vc-redist) をインストールします。[x64用](https://aka.ms/vc14/vc_redist.x64.exe) / [x86用](https://aka.ms/vc14/vc_redist.x86.exe) のうち、実行するEXE・DLLと同じものが必要です。両方利用するPCには両方入れます。

### 2. サーバーを配置する

サーバー用ZIPを、例として `C:\Apps\BonDriverRelay` に展開します。

```text
C:\Apps\BonDriverRelay\
  BonDriverRelayServer.exe
  BonDriverRelayProbe.exe
  server.toml
  Check-Config.cmd
  Start-Server.cmd
  client\
    BonDriver_Relay.dll
    BonDriver_Relay.toml
  licenses\
```

`BonDriver` フォルダーを作り、手元のチューナー用BonDriverと、そのBonDriverが必要とする設定ファイルを配置してください。`libaribb25.dll` やチューナー用DLLはZIPに含まれていません。

`server.toml` をテキストエディターで開き、末尾のチューナー定義を実際の配置に合わせます。次はPX4地上波用の例です。

```toml
[[tuners]]
name = "tuner-0"
group = "default"
dll_path = 'C:\Apps\BonDriverRelay\BonDriver\BonDriver_PX4-T0.dll'
```

パスは絶対パスで指定します。別の場所に展開した場合も変更してください。TOMLではWindowsのパスを上記のようにシングルクォートで囲めます。

複数の物理チューナーを使う場合は、個別に開けるBonDriverの構成を用意し、`[[tuners]]` を追加します。`name` は重複させません。同じチャンネル一覧を持つチューナーだけを同じ `group` にします。地上波とBS/CSなど、一覧が異なるものは別グループにしてください。

BonDriverの複製方法と、`.ini`・`.ChSet.txt` などの名前・共有方法は、そのBonDriverの仕様に従います。DLLを複製しただけで別の物理チューナーを使えるとは限りません。

### 3. サーバーを起動する

`Check-Config.cmd` をダブルクリックし、`Configuration valid` が表示されることを確認します。これは設定とDLLファイルの検査であり、チューナーの受信や復号を行うものではありません。

続いて `Start-Server.cmd` をダブルクリックします。`TCP port=57230` と表示されたら、ウィンドウを開いたまま次へ進みます。終了はそのウィンドウで `Ctrl+C` です。ログは `server.toml` と同じ場所の `logs` フォルダーに保存されます。

PowerShellから起動する場合は、次のコマンドでも同じです。

```powershell
& 'C:\Apps\BonDriverRelay\BonDriverRelayServer.exe' --serve --config 'C:\Apps\BonDriverRelay\server.toml'
```

**別PCから使う場合:** `server.toml` の `[network]` にある `bind_address` を、サーバーのLAN側IPv4アドレス（例: `192.168.1.10`）に変更して起動し直します。必要に応じてWindowsファイアウォールで、信頼するプライベートネットワークからTCP `57230` への受信を許可してください。初期設定の `127.0.0.1` は同じPCからだけ接続できます。

### 4. TVTest・EDCBに配置する

アプリと同じbit数のZIPにある `client` フォルダーから、次の2ファイルをアプリのBonDriver配置先にコピーします。TVTestでは `BonDriver` フォルダー、EDCBでは使用構成に応じたBonDriverフォルダーです。

```text
BonDriver_Relay.dll
BonDriver_Relay.toml
```

コピーした `BonDriver_Relay.toml` の該当箇所を編集します。

```toml
[server]
host = "127.0.0.1"
port = 57230
connect_timeout_ms = 5000
request_timeout_ms = 20000

[client]
tuner_group = "default"
```

`host` はサーバーのIPv4アドレスです。同じPCなら `127.0.0.1`、別PCなら例として `192.168.1.10` にします。ホスト名は指定できません。`tuner_group` はサーバー側の `group` と一致させます。**既存の `[server]` / `[client]` を編集し、同じテーブルを追加しないでください。** その他の設定は同梱ファイルのままで構いません。

アプリを起動し、BonDriverに `BonDriver_Relay.dll` を選択してチャンネルスキャン・選局を行います。チャンネル一覧と番号は、サーバー側のBonDriverが返すものです。アプリのリモコン番号や物理チャンネル番号と、BonDriverのチャンネル番号は一致するとは限りません。

グループ別に使う場合は、DLLとTOMLを同じベース名で複製します。例えば `BonDriver_Relay_BS.dll` には `BonDriver_Relay_BS.toml` を用意し、その `tuner_group` を変更します。

### 5. サーバー側B25復号を使う場合

復号せずに利用する場合は、この手順は不要です。

1. [tsukumijima/libaribb25 v0.2.10](https://github.com/tsukumijima/libaribb25/releases/tag/v0.2.10) のWindows用DLLを別途入手します。**サーバーEXEと同じbit数** の `libaribb25.dll` を、例として `C:\Apps\BonDriverRelay\libaribb25.dll` に配置します。
2. カード・カードリーダーと、その提供元が指定するドライバーをサーバーPCに用意します。PX4内蔵リーダーなど、専用の接続ライブラリが必要な構成はその提供元の手順に従ってください。
3. `server.toml` の既存の `[decoder]` を次のように編集し、サーバーを起動し直します。

```toml
[decoder]
enabled = true
dll_path = 'C:\Apps\BonDriverRelay\libaribb25.dll'
failure_policy = "stop_stream"
multi2_round = 4
simd_mode = 0
```

通常のWindowsスマートカードリーダーを使う場合、独自の `winscard.dll` を用意する必要はありません。接続方式に応じた信頼できる `winscard.dll` を別途使用する場合は、同じbit数のものをサーバーEXEの横に置き、プロセスを完全に終了してから起動してください。サーバーをWindowsサービスでも動かしている場合は先に停止します。WindowsのシステムフォルダーにあるDLLは置き換えないでください。

`stop_stream` は復号処理のエラー時に配信を停止します。`passthrough` にすると、エラー状態を通知しながら元のTSを配信します。B25が無効の場合や元TSの透過時には、受信アプリ側で必要な復号を行ってください。

### 6. 管理画面・複数サーバーを使う場合

`server.toml` の既存の `[monitoring]` で `enabled = true` にしてサーバーを起動し直すと、サーバーPCのブラウザーで [http://127.0.0.1:57231/](http://127.0.0.1:57231/) を開けます。チューナー、接続クライアント、TS量、キュー、ドロップ、デコーダー状態を表示します。JSONは `/api/local`・`/api/status`、Prometheus形式は `/metrics` です。

別サーバーの状態も表示する場合は、相手の監視機能を有効にしてLAN側アドレスへバインドし、参照する側の `server.toml` に以下を追加します。監視用TCP `57231` のアクセスも必要です。

```toml
[[monitoring.peers]]
name = "server-b"
host = "192.168.1.20"
port = 57231
```

クライアントが空きのある接続先を選べるようにするには、アプリ側の `BonDriver_Relay.toml` に候補を追加します。最大7件追加できます。各サーバーで同じグループ名・同じチャンネル一覧を用意してください。

```toml
[[fallback_servers]]
host = "192.168.1.20"
port = 57230
```

最初の接続先を開けない場合や、選局要求がチューナー不足で拒否された場合に、設定順に別の候補を試します。配信中のネットワーク断やサーバー停止から、継続中の録画を別サーバーへ自動復旧する機能はありません。

設定を起動中に変更したい場合は、`[reload]` の `enabled = true` を指定して起動します。反映できるのは `logging.level` と `logging.status_interval_ms` だけです。他の項目を同時に変更するとその更新全体を拒否します。チューナー・接続先・復号などを変更した場合は再起動してください。

### 7. Windowsサービスとして使う場合

コンソールを終了してから、管理者のPowerShellで登録します。サービス名は `BonDriverRelay` としてください。次の例では、現在PowerShellを実行しているWindowsアカウントをサービスにも使用します。

```powershell
$serviceUser = whoami
$serviceAccount = Get-Credential -UserName $serviceUser -Message 'サービス実行アカウントのWindowsパスワードを入力してください（PINではありません）'
$serviceBinary = '"C:\Apps\BonDriverRelay\BonDriverRelayServer.exe" --service --config "C:\Apps\BonDriverRelay\server.toml"'
New-Service -Name BonDriverRelay -DisplayName 'BonDriverRelay' -BinaryPathName $serviceBinary -StartupType Automatic -Credential $serviceAccount
Start-Service BonDriverRelay
```

別のアカウントを使う場合は、`$serviceUser` にそのアカウント名を指定してください。ローカルユーザーは `PC名\ユーザー名` または `.\ユーザー名`、ドメインユーザーは `ドメイン名\ユーザー名` の形式にします。表示名だけを入力しないでください。「指定されたドメインがないか、またはアクセスできません」と表示された場合は、まず `$serviceAccount.UserName` の表記と、そのアカウントが対象PCで利用できることを確認してください。

指定するアカウントには「サービスとしてログオン」の権利、EXE・設定・DLLの読み取り権限、`logs` フォルダーの書き込み権限、カードリーダーへのアクセス権限が必要です。カード接続方式がそのアカウント・サービス環境でも利用できるようにしてください。普段のユーザーで起動したアプリの環境はそのまま引き継がれません。

停止は `Stop-Service BonDriverRelay` です。削除する場合は停止後に `sc.exe delete BonDriverRelay` を実行します。更新時もサービス・アプリを停止してからファイルを差し替え、手元のTOML設定を保持してください。

## 注意点

- 認証・TLSはありません。信頼するLANまたはVPN内で使用してください。TS用・管理画面用の両ポートをインターネットに直接公開しないでください。管理画面も初期設定は無効・localhost限定です。
- サーバーEXE・サーバーが読み込むBonDriver・`libaribb25.dll`・必要な接続DLLのbit数を一致させてください。クライアントDLLはTVTest・EDCBと同じbit数にします。外部DLLは信頼できる入手元のものだけを使用してください。
- 物理チューナー数を超える別チャンネルの同時受信はできません。同一グループ・同一チャンネルは共有します。優先度を設定しても、受信中の別クライアントを追い出しません。
- 受信アプリやネットワークが遅くキューがあふれると、初期設定ではその購読を切断します。`drop_oldest` を選ぶとTSが欠落するため、録画用途には適しません。切断後はアプリでBonDriverを開き直してください。
- 外部DLLの応答が設定した起動・終了期限を超えた場合、サーバープロセスを終了コード1460で終了します。サーバーの停止・再起動は配信中の視聴・録画に影響します。
- クライアントDLLのAPIは `IBonDriver` / `IBonDriver2` です。`IBonDriver3` のデバイス数取得・LNB電源制御は提供しません。サーバー側BonDriverに必要なLNB電源などは、そのBonDriverの設定で指定してください。
- BonDriverProxyExDの通信プロトコルとは互換性がありません。旧クライアントや `BonDriver_mirakc` の接続先にはできません。
- BonDriverRelayは [MIT License](LICENSE) です。第三者ライブラリ・外部DLLにはそれぞれのライセンスが適用されます。帰属情報は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) と同梱の `licenses` を参照してください。

#include "BonDriverRelay/monitor/MonitorView.h"
#include "BonDriverRelay/Version.h"
#include <sstream>
namespace bdr::monitor
{
    std::string JsonString(std::string_view text)
    {
        constexpr char HEX[] = "0123456789abcdef";
        std::string output = "\"";
        for (unsigned char ch : text) {
            if (ch == '"' || ch == '\\') {
                output += '\\';
                output += static_cast<char>(ch);
            }
            else if (ch < 32) {
                output += "\\u00";
                output += HEX[ch >> 4];
                output += HEX[ch & 15];
            }
            else {
                output += static_cast<char>(ch);
            }
        }
        return output + '"';
    }
    std::string LocalJson(const std::string& name, const core::ServerSnapshot& snapshot,
                          const core::ReloadSnapshot& reload)
    {
        std::ostringstream out;
        out << "{\"name\":" << JsonString(name)
            << ",\"version\":\"" BDR_VERSION "\",\"connections\":" << snapshot.activeConnections
            << ",\"accepted\":" << snapshot.acceptedConnections << ",\"rejected\":" << snapshot.rejectedConnections
            << ",\"requests\":" << snapshot.requests << ",\"requestErrors\":" << snapshot.requestErrors
            << ",\"sentTsBytes\":" << snapshot.sentTsBytes << ",\"queuedBytes\":" << snapshot.manager.queuedBytes
            << ",\"droppedPackets\":" << snapshot.manager.droppedPackets
            << ",\"reload\":{\"enabled\":" << (reload.enabled ? "true" : "false") << ",\"applied\":" << reload.applied
            << ",\"rejected\":" << reload.rejected << ",\"level\":" << JsonString(reload.level)
            << ",\"statusIntervalMs\":" << reload.statusIntervalMs << ",\"lastError\":" << JsonString(reload.lastError)
            << "},\"tuners\":[";
        bool first = true;
        for (const auto& tuner : snapshot.manager.tuners) {
            if (!first) {
                out << ',';
            }
            first = false;
            out << "{\"name\":" << JsonString(tuner.name) << ",\"phase\":" << static_cast<std::uint32_t>(tuner.phase)
                << ",\"subscribers\":" << tuner.subscribers << ",\"receivedBytes\":" << tuner.receivedBytes
                << ",\"outputBytes\":" << tuner.outputBytes << ",\"decoderState\":" << tuner.decoderState
                << ",\"decoderFailures\":" << tuner.decoderFailures << ",\"tuneFailures\":" << tuner.tuneFailures
                << ",\"cleanupFailures\":" << tuner.cleanupFailures << ",\"queuedBytes\":" << tuner.queuedBytes << '}';
        }
        out << "],\"clients\":[";
        first = true;
        for (const auto& client : snapshot.manager.clients) {
            if (!first) {
                out << ',';
            }
            first = false;
            out << "{\"id\":" << client.id << ",\"group\":" << JsonString(client.group)
                << ",\"tuner\":" << JsonString(client.tuner)
                << ",\"exclusive\":" << (client.exclusive ? "true" : "false")
                << ",\"pending\":" << (client.pending ? "true" : "false") << ",\"queuedBytes\":" << client.queuedBytes
                << ",\"droppedPackets\":" << client.droppedPackets << '}';
        }
        return out.str() + "]}";
    }
    std::string StatusJson(const std::string& local, const std::vector<PeerSnapshot>& peers)
    {
        std::string output = "{\"local\":" + local + ",\"peers\":[";
        bool first = true;
        for (const auto& peer : peers) {
            if (!first) {
                output += ',';
            }
            first = false;
            output += "{\"name\":" + JsonString(peer.name) + ",\"host\":" + JsonString(peer.host) +
                      ",\"port\":" + std::to_string(peer.port) + ",\"up\":" + (peer.up ? "true" : "false") +
                      ",\"ageMs\":" + std::to_string(peer.ageMs) + ",\"statusJson\":" + JsonString(peer.statusJson) +
                      '}';
        }
        return output + "]}";
    }
    std::string Metrics(const std::string& name, const core::ServerSnapshot& snapshot,
                        const core::ReloadSnapshot& reload, const std::vector<PeerSnapshot>& peers)
    {
        std::ostringstream out;
        // Names originate in validated config; JSON escaping also covers Prometheus backslash/quote labels.
        const auto labels = "server=" + JsonString(name);
        const auto metric = [&](const char* key, const char* type, std::uint64_t value) {
            out << "# TYPE bdr_" << key << ' ' << type << "\nbdr_" << key << '{' << labels << "} " << value << '\n';
        };
        metric("connections", "gauge", snapshot.activeConnections);
        metric("connections_accepted_total", "counter", snapshot.acceptedConnections);
        metric("connections_rejected_total", "counter", snapshot.rejectedConnections);
        metric("requests_total", "counter", snapshot.requests);
        metric("request_errors_total", "counter", snapshot.requestErrors);
        metric("sent_ts_bytes_total", "counter", snapshot.sentTsBytes);
        metric("queued_bytes", "gauge", snapshot.manager.queuedBytes);
        metric("dropped_packets_total", "counter", snapshot.manager.droppedPackets);
        metric("reload_applied_total", "counter", reload.applied);
        metric("reload_rejected_total", "counter", reload.rejected);
        const auto family = [&](const char* key, const char* type, auto value) {
            out << "# TYPE bdr_tuner_" << key << ' ' << type << '\n';
            for (const auto& tuner : snapshot.manager.tuners) {
                out << "bdr_tuner_" << key << '{' << labels << ",tuner=" << JsonString(tuner.name) << "} "
                    << value(tuner) << '\n';
            }
        };
        family("phase", "gauge", [](const auto& t) { return static_cast<std::uint32_t>(t.phase); });
        family("subscribers", "gauge", [](const auto& t) { return t.subscribers; });
        family("received_bytes_total", "counter", [](const auto& t) { return t.receivedBytes; });
        family("output_bytes_total", "counter", [](const auto& t) { return t.outputBytes; });
        family("decoder_state", "gauge", [](const auto& t) { return t.decoderState; });
        family("decoder_failures_total", "counter", [](const auto& t) { return t.decoderFailures; });
        family("tune_failures_total", "counter", [](const auto& t) { return t.tuneFailures; });
        family("cleanup_failures_total", "counter", [](const auto& t) { return t.cleanupFailures; });
        family("queued_bytes", "gauge", [](const auto& t) { return t.queuedBytes; });
        out << "# TYPE bdr_peer_up gauge\n";
        for (const auto& peer : peers) {
            out << "bdr_peer_up{" << labels << ",peer=" << JsonString(peer.name) << "} " << (peer.up ? 1 : 0) << '\n';
        }
        return out.str();
    }
    std::string_view DashboardHtml()
    {
        return R"HTML(<!doctype html>
<html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>BonDriverRelay 管理画面</title><link rel="stylesheet" href="/style.css">
<h1>BonDriverRelay</h1><p>サーバー・チューナー・接続状況</p>
<p><button id="refresh" type="button">更新</button> <label><input id="auto" type="checkbox" checked>5秒ごとに更新</label>
 · <a href="/metrics">メトリクス</a> · <a href="/api/status">JSON</a></p>
<p id="result" role="status">取得中…</p><main id="servers"></main><script src="/app.js"></script></html>)HTML";
    }
    std::string_view DashboardStyle()
    {
        return "body{font-family:system-ui,sans-serif;margin:1.5em;max-width:1100px}table{border-collapse:collapse;"
               "margin:1em 0}th,td{border:1px solid #aaa;padding:.35em .6em;text-align:left}section{margin:2em "
               "0}pre{white-space:pre-wrap}";
    }
    std::string_view DashboardScript()
    {
        return R"JS('use strict';
const el=(tag,text)=>{const node=document.createElement(tag);if(text!==undefined)node.textContent=String(text);return node;};
function table(parent,headers,rows){const t=el('table'),head=el('tr');for(const h of headers)head.append(el('th',h));const thead=el('thead');thead.append(head);t.append(thead);const body=el('tbody');for(const row of rows){const tr=el('tr');for(const cell of row)tr.append(el('td',cell??'—'));body.append(tr);}if(!rows.length){const tr=el('tr'),td=el('td','なし');td.colSpan=headers.length;tr.append(td);body.append(tr);}t.append(body);parent.append(t);}
const phases=['待機','選局中','受信中','障害','停止'];
function server(parent,s,label){if(!s||!Array.isArray(s.tuners)||!Array.isArray(s.clients))throw Error('状態データの形式が不正です');const section=el('section');section.append(el('h2',label),el('p',`${s.version} / 接続 ${s.connections} / 送信TS ${s.sentTsBytes} bytes / キュー ${s.queuedBytes} bytes / 欠損 ${s.droppedPackets} packets`));
table(section,['チューナー','状態','購読数','受信 bytes','出力 bytes','復号状態','復号失敗','選局失敗','解放失敗'],s.tuners.map(t=>[t.name,phases[t.phase]??t.phase,t.subscribers,t.receivedBytes,t.outputBytes,t.decoderState,t.decoderFailures,t.tuneFailures,t.cleanupFailures]));
section.append(el('h3','クライアント'));table(section,['ID','グループ','チューナー','排他','選局待ち','キュー bytes','欠損 packets'],s.clients.map(c=>[c.id,c.group,c.tuner||'未選局',c.exclusive?'はい':'いいえ',c.pending?'はい':'いいえ',c.queuedBytes,c.droppedPackets]));
if(s.reload)section.append(el('p',`設定再読込: ${s.reload.enabled?'有効':'無効'} / 適用 ${s.reload.applied} / 拒否 ${s.reload.rejected} / ログ ${s.reload.level}`));if(s.reload?.lastError)section.append(el('pre',s.reload.lastError));parent.append(section);}
let busy=false;async function refresh(){if(busy)return;busy=true;const result=document.getElementById('result');try{const r=await fetch('/api/status',{cache:'no-store',signal:AbortSignal.timeout(4000)});if(!r.ok)throw Error(`HTTP ${r.status}`);const data=await r.json(),fragment=document.createDocumentFragment();server(fragment,data.local,data.local.name+'（このサーバー）');for(const p of data.peers){if(p.up){try{server(fragment,JSON.parse(p.statusJson),`${p.name}（取得から ${Math.floor(p.ageMs/1000)} 秒）`);}catch{fragment.append(el('p',p.name+': 状態データを読み取れません'));}}else fragment.append(el('p',p.name+': 接続できません（チューナー状態不明）'));}document.getElementById('servers').replaceChildren(fragment);result.textContent='更新: '+new Date().toLocaleString();}catch(e){document.getElementById('servers').replaceChildren();result.textContent='取得失敗: '+e.message;}finally{busy=false;}}
document.getElementById('refresh').addEventListener('click',refresh);setInterval(()=>{if(document.getElementById('auto').checked)refresh();},5000);refresh();
)JS";
    }
} // namespace bdr::monitor

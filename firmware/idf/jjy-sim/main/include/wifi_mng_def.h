#ifndef _WIFI_AP_DEF_H_
#define _WIFI_AP_DEF_H_

#define WIFI_AP_TITLE			"JJY-SIM R%d Setup"
#define WIFI_AP_SSID_PREFIX		"JJY-SIM-R%d-"
#define	WIFI_AP_PASS			""

#define WIFI_AP_BUTTON_CSS_STYLE \
    "button{font-size:16px;padding:10px 12px;margin-top:4px;border-radius:8px;box-sizing:border-box;background:#2d6cdf;color:#fff;border:none;cursor:pointer;min-width:180px;}" \
    "button:hover{opacity:0.94;}"

#define WIFI_AP_CSS_STYLE \
    "body{font-family:sans-serif;background:#f5f7fa;color:#222;max-width:820px;margin:0 auto;padding:20px 16px;line-height:1.5;}" \
    "h1{font-size:24px;margin:0 0 8px 0;}" \
    "h2{font-size:20px;margin:0 0 12px 0;}" \
    "p.sub{margin:0 0 18px 0;color:#666;font-size:14px;}" \
    "fieldset{border:1px solid #d7dce2;border-radius:10px;background:#fff;margin:16px 0;padding:16px;box-shadow:0 1px 3px rgba(0,0,0,0.04);}" \
    "legend{padding:0 8px;font-weight:700;color:#333;}" \
    "label{display:block;margin-top:10px;font-size:14px;color:#444;}" \
    "input,select{font-size:16px;padding:10px 12px;margin-top:4px;border:1px solid #bfc7d1;border-radius:8px;box-sizing:border-box;}" \
    "input,select{width:100%;background:#f0f6ff;border:1px solid #7aa7ff;}" \
    "select:focus{background:#e6f0ff;border-color:#2d6cdf;}" \
    "option{background:#fff;color:#222;}" \
    WIFI_AP_BUTTON_CSS_STYLE \
    "table{width:100%;border-collapse:collapse;}" \
    "th,td{padding:8px 0;vertical-align:top;border-bottom:1px solid #eef1f4;}" \
    "th{width:150px;text-align:left;color:#555;font-weight:600;white-space:nowrap;padding-right:12px;}" \
    "td{color:#222;word-break:normal;overflow-wrap: break-all;}" \
    ".mono{font-family:Consolas,'Courier New',monospace;}" \
    ".actions{margin-top:18px;}" \
    ".scan-overlay[hidden]{display:none;}" \
    ".scan-overlay{position:fixed;top:0;right:0;bottom:0;left:0;z-index:1000;background:rgba(0,0,0,0.4);display:flex;align-items:center;justify-content:center;padding:20px;}" \
    ".scan-dialog{background:#fff;border-radius:12px;padding:24px;text-align:center;max-width:360px;width:100%;box-shadow:0 8px 30px rgba(0,0,0,0.2);}" \
    ".scan-dialog h2{font-size:20px;}" \
    ".scan-dialog p{margin-bottom:0;}" \
    ".scan-spinner{width:30px;height:30px;margin:0 auto 16px;border:4px solid #dce5f5;border-top-color:#2d6cdf;border-radius:50%;animation:scan-spin 1s linear infinite;}" \
    "@keyframes scan-spin{to{transform:rotate(360deg);}}" \
    ".actions form{margin:10px 0;}" \
    ".nav{margin-top:18px;}" \
    ".nav a{display:inline-block;padding:10px 14px;border:1px solid #c8d0da;border-radius:8px;background:#fff;color:#2d6cdf;text-decoration:none;}" \
    ".nav a:hover{text-decoration:none;background:#f8fbff;}" \
    ".footer-note{margin-top:18px;font-size:13px;color:#666;}" \
    "input[type=range]{width:100%;}" \
    ".checkrow{display:flex;align-items:center;gap:8px;margin-top:10px;font-size:14px;color:#444;}" \
    ".checkrow input[type=checkbox]{width:auto;margin:0;padding:0;}" \
    ".checkrow label{display:inline;margin:0;}" \
    ".radiorow{display:flex;align-items:center;gap:8px;}" \
    ".radiorow input[type=radio]{width:auto;margin:0;padding:0;}" \
    ".ntp-label{min-width:160px;}" \
    ".ntp-input{margin-top:8px;}" \
    ".setting{display:flex;align-items:center;gap:8px;font-size:14px;flex-wrap:nowrap;}"\
    ".setting label,.setting span {white-space:nowrap;}"\
    "input[type=number]{width:50px;flex:none;}"\
    ".disabled{opacity:0.5;}"\
    ""

#define WIFI_AP_MIT_LICENSE \
    "MIT License\n\n" \
    "Copyright (c) 2026 Shachi-lab\n\n" \
    "Permission is hereby granted, free of charge, to any person obtaining a copy " \
    "of this software and associated documentation files (the \"Software\"), to deal " \
    "in the Software without restriction, including without limitation the rights " \
    "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell " \
    "copies of the Software, and to permit persons to whom the Software is " \
    "furnished to do so, subject to the following conditions:\n\n" \
    "The above copyright notice and this permission notice shall be included in all " \
    "copies or substantial portions of the Software.\n\n" \
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR " \
    "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, " \
    "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE " \
    "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER " \
    "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM," \
    "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE " \
    "SOFTWARE.\n"

#endif

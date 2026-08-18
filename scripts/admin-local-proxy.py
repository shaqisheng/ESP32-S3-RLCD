#!/usr/bin/env python3
"""admin-local-proxy.py — 通过 localhost 访问 RLCD 后台（获得安全上下文）。

为什么需要它：后台在 HTTP 局域网上是非安全上下文（isSecureContext=false），
浏览器禁止 navigator.clipboard.write，截图面板的「复制」按钮只能写 text/html
文本味（部分粘贴目标只显示文字）。localhost 在浏览器安全模型里是可信上下文，
经本代理访问后 ClipboardItem API 生效，「复制」按钮写入的是**原生 PNG 图片**，
任何应用都能粘贴。

用法：
    python3 scripts/admin-local-proxy.py [设备IP] [本地端口]
    # 默认设备 192.168.40.118、本地端口 8080
然后浏览器打开：
    http://localhost:8080/admin

零依赖（仅标准库），Ctrl+C 停止。设备 IP 会变（DHCP 会重新分配），
以 arp -a | grep -i espressif 或设备启动日志 Got IP 为准。
"""

import socket
import sys
import threading

BUFFER = 65536


def pump(src: socket.socket, dst: socket.socket) -> None:
    try:
        while True:
            data = src.recv(BUFFER)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        for sock in (src, dst):
            try:
                sock.close()
            except OSError:
                pass


def main() -> None:
    device = sys.argv[1] if len(sys.argv) > 1 else "192.168.40.118"
    local_port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", local_port))
    server.listen(16)
    print(f"转发 http://localhost:{local_port} → http://{device}:8080（Ctrl+C 停止）")
    print("用上面的 localhost 地址打开后台，「复制」按钮即可写入真实图片。")

    while True:
        client, _ = server.accept()
        try:
            upstream = socket.create_connection((device, 8080), timeout=10)
        except OSError as exc:
            print(f"无法连接设备 {device}:8080：{exc}")
            client.close()
            continue
        threading.Thread(target=pump, args=(client, upstream), daemon=True).start()
        threading.Thread(target=pump, args=(upstream, client), daemon=True).start()


if __name__ == "__main__":
    main()

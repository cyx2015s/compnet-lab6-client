#!/usr/bin/env python3
import socket
import sys
import threading
import time

class PortListener:
    def __init__(self, port, buffer_size=4096):
        self.port = port
        self.buffer_size = buffer_size
        self.server_socket = None
        self.running = False
        
    def send_test_packets(self, client_socket):
        """发送各种类型的数据包以便测试"""
        try:
            # 1. 请求当前时间响应 (0x81)
            timestamp_packet = b'\x81\x00\x00\x08' + int(time.time()).to_bytes(8, 'big')
            client_socket.sendall(timestamp_packet)

            # 2. 名称响应 (0x82)
            server_name = "TestServer"
            name_packet = b'\x82\x00' + (len(server_name) + 2).to_bytes(2, 'big') + (len(server_name)).to_bytes(2, 'big') + server_name.encode('utf-8')
            print(name_packet[2:3], name_packet[4:5])
            print(name_packet)
            client_socket.sendall(name_packet)

            # 3. 活动连接列表响应 (0x83)
            client_count = 1
            client_id = 12345
            ipv4 = socket.inet_aton("127.0.0.1")

            port = 8080
            connection_list_packet = (
                b'\x83\x00\x00\x0e' +
                client_count.to_bytes(2, 'big') +
                client_id.to_bytes(4, 'big') +
                ipv4 +
                port.to_bytes(2, 'big')
            )
            actual_payload_size = (len(connection_list_packet) - 4).to_bytes(2, 'big')
            connection_list_packet = connection_list_packet[:2] + actual_payload_size + connection_list_packet[4:]
            client_socket.sendall(connection_list_packet)

            # 4. 转发消息 (0x84)
            from_id = 54321
            message = "Hello, Client!"
            forward_packet = (
                b'\x84\x00' +
                (4 + 2 + len(message)).to_bytes(2, 'big') +
                from_id.to_bytes(4, 'big') +
                len(message).to_bytes(2, 'big') +
                message.encode('utf-8')
            )
            client_socket.sendall(forward_packet)

            # 5. 错误响应 (0xFF)
            error_code = 1
            error_message = "Unknown message type"
            error_packet = (
                b'\xFF\x00' +
                (2 + len(error_message)).to_bytes(2, 'big') +
                error_code.to_bytes(2, 'big') +
                error_message.encode('utf-8')
            )
            client_socket.sendall(error_packet)

        except Exception as e:
            print(f"[-] 发送测试数据包时出错: {e}")

    def handle_client(self, client_socket, client_address):
        """处理客户端连接"""
        print(f"[+] 新连接来自: {client_address[0]}:{client_address[1]}")
        
        try:
            # client_socket.sendall("欢迎连接到端口监听器服务器!\n".encode('utf-8'))
            
            # 发送测试数据包
            for _ in range(10):
                self.send_test_packets(client_socket)
                threading.Event().wait(0.5)
            
            while self.running:
                try:
                    # 接收数据
                    data = client_socket.recv(self.buffer_size)
                    
                    if not data:
                        print(f"[-] 连接断开: {client_address[0]}:{client_address[1]}")
                        break
                    
                    # 尝试解码为UTF-8文本
                    try:
                        message = data.decode('utf-8').strip()
                        print(f"[{client_address[0]}:{client_address[1]}] 消息: {message}")
                    except UnicodeDecodeError:
                        pass
                        # 如果是二进制数据，显示十六进制
                    
                    hex_data = data.hex()
                    print(f"[{client_address[0]}:{client_address[1]}] 二进制数据 ({len(data)} 字节): {hex_data}")
                    
                except socket.timeout:
                    # client_socket.sendall('还回家吃饭吗？\n'.encode('utf-8'))
                    continue
                except ConnectionResetError:
                    print(f"[-] 连接被重置: {client_address[0]}:{client_address[1]}")
                    break
                except Exception as e:
                    print(f"[-] 接收错误: {e}")
                    break
                    
        finally:
            client_socket.close()
    
    def start(self):
        """启动服务器"""
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        
        # 绑定到所有网络接口的指定端口
        self.server_socket.bind(('0.0.0.0', self.port))
        self.server_socket.listen(5)
        self.server_socket.settimeout(1.0)  # 设置超时以便可以检查运行状态
        
        self.running = True
        
        print(f"[*] 服务器启动，监听端口 {self.port}")
        print(f"[*] 监听地址: 0.0.0.0 (所有网络接口)")
        print(f"[*] 按 Ctrl+C 停止服务器\n")
        
        try:
            while self.running:
                try:
                    client_socket, client_address = self.server_socket.accept()
                    client_socket.settimeout(1.0)
                    
                    # 为每个客户端创建新线程
                    client_thread = threading.Thread(
                        target=self.handle_client,
                        args=(client_socket, client_address),
                        daemon=True
                    )
                    client_thread.start()
                    
                except socket.timeout:
                    continue
                    
        except KeyboardInterrupt:
            print("\n[*] 收到停止信号，正在关闭服务器...")
        except Exception as e:
            print(f"[-] 服务器错误: {e}")
        finally:
            self.stop()
    
    def stop(self):
        """停止服务器"""
        self.running = False
        if self.server_socket:
            self.server_socket.close()
        print("[*] 服务器已停止")

def main():
    if len(sys.argv) != 2:
        print("使用方法: python port_listener.py <端口号>")
        print("示例: python port_listener.py 8080")
        sys.exit(1)
    
    try:
        port = int(sys.argv[1])
        if not 1 <= port <= 65535:
            print("错误: 端口号必须在 1-65535 范围内")
            sys.exit(1)
    except ValueError:
        print("错误: 端口号必须是整数")
        sys.exit(1)
    
    listener = PortListener(port)
    
    try:
        listener.start()
    except Exception as e:
        print(f"[-] 启动失败: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
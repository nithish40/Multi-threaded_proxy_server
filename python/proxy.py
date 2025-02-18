import socket
import threading
import os
import time
import select
import requests
import pickle
import logging
from datetime import datetime
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import urlparse, urljoin

# Constants
CACHE_DIR = "cache"
LOGS_DIR = "logs"
BUFFER_SIZE = 8192
MAX_WORKERS = 50
DEFAULT_TIMEOUT = 30

class ProxyLogger:
    def __init__(self):
        # Create logs directory if it doesn't exist
        os.makedirs(LOGS_DIR, exist_ok=True)
        
        # Set up main logger
        self.logger = logging.getLogger('ProxyServer')
        self.logger.setLevel(logging.INFO)
        
        # Create handlers for different log types
        self._setup_access_logger()
        self._setup_error_logger()
        self._setup_security_logger()
        
        # Traffic statistics
        self.stats = {
            'total_requests': 0,
            'blocked_requests': 0,
            'bytes_transferred': 0
        }
    
    def _setup_access_logger(self):
        # Access log format: timestamp | client_ip | method | url | status_code | bytes
        access_handler = logging.FileHandler(os.path.join(LOGS_DIR, 'access.log'))
        access_handler.setLevel(logging.INFO)
        access_formatter = logging.Formatter('%(asctime)s | %(message)s')
        access_handler.setFormatter(access_formatter)
        
        self.access_logger = logging.getLogger('ProxyServer.access')
        self.access_logger.setLevel(logging.INFO)
        self.access_logger.addHandler(access_handler)
    
    def _setup_error_logger(self):
        error_handler = logging.FileHandler(os.path.join(LOGS_DIR, 'error.log'))
        error_handler.setLevel(logging.ERROR)
        error_formatter = logging.Formatter('%(asctime)s | %(levelname)s | %(message)s')
        error_handler.setFormatter(error_formatter)
        
        self.error_logger = logging.getLogger('ProxyServer.error')
        self.error_logger.setLevel(logging.ERROR)
        self.error_logger.addHandler(error_handler)
    
    def _setup_security_logger(self):
        security_handler = logging.FileHandler(os.path.join(LOGS_DIR, 'security.log'))
        security_handler.setLevel(logging.WARNING)
        security_formatter = logging.Formatter('%(asctime)s | SECURITY | %(message)s')
        security_handler.setFormatter(security_formatter)
        
        self.security_logger = logging.getLogger('ProxyServer.security')
        self.security_logger.setLevel(logging.WARNING)
        self.security_logger.addHandler(security_handler)
    
    def log_access(self, client_ip, method, url, status_code, bytes_transferred):
        self.stats['total_requests'] += 1
        self.stats['bytes_transferred'] += bytes_transferred
        self.access_logger.info(f"{client_ip} | {method} | {url} | {status_code} | {bytes_transferred}")
    
    def log_error(self, client_ip, message, exception=None):
        error_msg = f"{client_ip} | {message}"
        if exception:
            error_msg += f" | Exception: {str(exception)}"
        self.error_logger.error(error_msg)
    
    def log_security(self, client_ip, message):
        self.stats['blocked_requests'] += 1
        self.security_logger.warning(f"{client_ip} | {message}")
    
    def get_statistics(self):
        return {
            'total_requests': self.stats['total_requests'],
            'blocked_requests': self.stats['blocked_requests'],
            'bytes_transferred': self.stats['bytes_transferred'],
            'bytes_transferred_formatted': self._format_bytes(self.stats['bytes_transferred'])
        }
    
    def _format_bytes(self, bytes):
        for unit in ['B', 'KB', 'MB', 'GB']:
            if bytes < 1024:
                return f"{bytes:.2f} {unit}"
            bytes /= 1024
        return f"{bytes:.2f} TB"

class Proxy:
    def __init__(self, port):
        self.port = port
        self.server_socket = None
        self.run = True
        self.cache_file = {}
        self.block_sites = {}
        self.servicing_threads = []
        self.logger = ProxyLogger()
        
        # Ensure directories exist
        os.makedirs(CACHE_DIR, exist_ok=True)
        
        # Load cached and blocked sites
        self.load_cache()
        self.load_blocked_sites()
        
        # Initialize thread pool
        self.thread_pool = ThreadPoolExecutor(max_workers=MAX_WORKERS)
        
        # Start management interface
        threading.Thread(target=self.dynamic_manager, daemon=True).start()

    def load_cache(self):
        try:
            if os.path.exists("cached_sites.pkl"):
                with open("cached_sites.pkl", "rb") as f:
                    self.cache_file = pickle.load(f)
        except Exception as e:
            self.logger.log_error("system", "Cache loading error", e)
            self.cache_file = {}

    def load_blocked_sites(self):
        try:
            if os.path.exists("blocked_sites.pkl"):
                with open("blocked_sites.pkl", "rb") as f:
                    self.block_sites = pickle.load(f)
        except Exception as e:
            self.logger.log_error("system", "Blocked sites loading error", e)
            self.block_sites = {}

    def save_cache(self):
        with open("cached_sites.pkl", "wb") as f:
            pickle.dump(self.cache_file, f)

    def save_blocked_sites(self):
        with open("blocked_sites.pkl", "wb") as f:
            pickle.dump(self.block_sites, f)

    def listen(self):
        try:
            self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.server_socket.bind(("", self.port))
            self.server_socket.listen(100)
            print(f"Proxy server listening on port {self.port}")

            while self.run:
                try:
                    client_socket, addr = self.server_socket.accept()
                    client_socket.settimeout(DEFAULT_TIMEOUT)
                    print(f"Connection from {addr}")
                    self.thread_pool.submit(self.handle_client, client_socket, addr[0])
                except Exception as e:
                    self.logger.log_error("system", "Error accepting connection", e)

        except Exception as e:
            self.logger.log_error("system", "Server error", e)
        finally:
            self.close_server()

    def handle_client(self, client_socket, client_ip):
        try:
            handler = RequestHandler(client_socket, self, client_ip)
            handler.handle_request()
        except Exception as e:
            self.logger.log_error(client_ip, "Error handling client", e)
        finally:
            try:
                client_socket.close()
            except:
                pass

    def close_server(self):
        print("Shutting down proxy server...")
        self.run = False
        self.save_cache()
        self.save_blocked_sites()
        self.thread_pool.shutdown(wait=True)
        
        if self.server_socket:
            try:
                self.server_socket.close()
            except:
                pass

    def dynamic_manager(self):
        while self.run:
            try:
                print("\nProxy Menu:")
                print("1. Block a site")
                print("2. View blocked sites")
                print("3. View cached sites")
                print("4. View statistics")
                print("5. Clear cache")
                print("6. View recent logs")
                print("7. Exit")
                
                choice = input("Enter choice (1-7): ").strip()
                
                if choice == "1":
                    site = input("Enter site to block: ").strip()
                    self.block_sites[site] = True
                    print(f"Blocked {site}")
                    self.save_blocked_sites()
                elif choice == "2":
                    print("Blocked sites:", list(self.block_sites.keys()))
                elif choice == "3":
                    print("Cached sites:", list(self.cache_file.keys()))
                elif choice == "4":
                    stats = self.logger.get_statistics()
                    print("\nProxy Statistics:")
                    print(f"Total Requests: {stats['total_requests']}")
                    print(f"Blocked Requests: {stats['blocked_requests']}")
                    print(f"Total Data Transferred: {stats['bytes_transferred_formatted']}")
                elif choice == "5":
                    self.cache_file.clear()
                    self.save_cache()
                    print("Cache cleared")
                elif choice == "6":
                    self.show_recent_logs()
                elif choice == "7":
                    self.close_server()
                    break
            except Exception as e:
                self.logger.log_error("system", "Menu error", e)

    def show_recent_logs(self, lines=10):
        print("\nRecent Access Logs:")
        self._show_last_lines(os.path.join(LOGS_DIR, 'access.log'), lines)
        
        print("\nRecent Error Logs:")
        self._show_last_lines(os.path.join(LOGS_DIR, 'error.log'), lines)
        
        print("\nRecent Security Logs:")
        self._show_last_lines(os.path.join(LOGS_DIR, 'security.log'), lines)

    def _show_last_lines(self, filename, lines):
        try:
            if os.path.exists(filename):
                with open(filename, 'r') as f:
                    content = f.readlines()
                    for line in content[-lines:]:
                        print(line.strip())
            else:
                print(f"No logs found in {filename}")
        except Exception as e:
            print(f"Error reading logs: {e}")

class RequestHandler:
    def __init__(self, client_socket, proxy, client_ip):
        self.client_socket = client_socket
        self.proxy = proxy
        self.client_ip = client_ip
        self.request_data = b""
        self.headers = {}
        self.bytes_transferred = 0

    def handle_request(self):
        try:
            # Receive initial request
            while b"\r\n\r\n" not in self.request_data:
                data = self.client_socket.recv(BUFFER_SIZE)
                if not data:
                    return
                self.request_data += data

            # Parse the request
            request_lines = self.request_data.split(b"\r\n")
            request_line = request_lines[0].decode()
            
            try:
                method, url, version = request_line.split(" ")
            except ValueError:
                self.proxy.logger.log_error(self.client_ip, f"Invalid request line: {request_line}")
                return

            # Parse headers
            for line in request_lines[1:]:
                try:
                    if line and b":" in line:
                        key, value = line.decode().split(":", 1)
                        self.headers[key.strip().lower()] = value.strip()
                except Exception as e:
                    self.proxy.logger.log_error(self.client_ip, "Header parsing error", e)

            # Handle different request types
            if method == "CONNECT":
                self.handle_https_request(url)
            else:
                self.handle_http_request(method, url)

        except Exception as e:
            self.proxy.logger.log_error(self.client_ip, "Request handling error", e)

    def handle_http_request(self, method, url):
        try:
            # Ensure URL is absolute
            if not url.startswith('http'):
                host = self.headers.get('host', '')
                url = f'http://{host}{url}'

            # Check if site is blocked
            parsed_url = urlparse(url)
            if parsed_url.netloc in self.proxy.block_sites:
                self.proxy.logger.log_security(self.client_ip, f"Blocked access to {url}")
                self.send_blocked_response()
                return

            # Create request
            request_headers = {
                'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36',
                'Accept': '*/*',
                'Accept-Encoding': 'identity',
                'Connection': 'close'
            }

            # Add original headers
            for key, value in self.headers.items():
                if key not in ['proxy-connection', 'connection']:
                    request_headers[key] = value

            # Make request
            response = requests.request(
                method=method,
                url=url,
                headers=request_headers,
                stream=True,
                verify=False,
                allow_redirects=True
            )

            # Send response headers
            header_string = f"HTTP/{response.raw.version / 10.0} {response.status_code} {response.reason}\r\n"
            for key, value in response.headers.items():
                header_string += f"{key}: {value}\r\n"
            header_string += "\r\n"
            
            self.client_socket.sendall(header_string.encode())

            # Stream response body
            for chunk in response.iter_content(chunk_size=BUFFER_SIZE):
                if chunk:
                    self.client_socket.sendall(chunk)
                    self.bytes_transferred += len(chunk)

            # Log the successful request
            self.proxy.logger.log_access(
                self.client_ip,
                method,
                url,
                response.status_code,
                self.bytes_transferred
            )

        except Exception as e:
            self.proxy.logger.log_error(self.client_ip, f"HTTP request error for {url}", e)
            self.send_error_response(502)

    def handle_https_request(self, url):
        try:
            host, port = url.split(':')
            port = int(port)

            # Log HTTPS connection attempt
            self.proxy.logger.log_access(self.client_ip, "CONNECT", url, 200, 0)

            # Create connection to remote server
            server_socket = socket.create_connection((host, port), timeout=DEFAULT_TIMEOUT)
            
            # Send connection established response
            self.client_socket.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")

            # Set up bidirectional tunneling
            self.tunnel_traffic(self.client_socket, server_socket)

        except Exception as e:
            self.proxy.logger.log_error(self.client_ip, f"HTTPS request error for {url}", e)
            self.send_error_response(502)

    def tunnel_traffic(self, client_socket, server_socket):
        try:
            sockets = [client_socket, server_socket]
            timeout = 1
            while True:
                readable, _, exceptional = select.select(sockets, [], sockets, timeout)

                if exceptional:
                    break

                for sock in readable:
                    other = server_socket if sock is client_socket else client_socket
                    try:
                        data = sock.recv(BUFFER_SIZE)
                        if not data:
                            return
                        other.sendall(data)
                    except Exception as e:
                        print(f"Tunneling error: {e}")
                        return

        except Exception as e:
            print(f"Tunnel error: {e}")
        finally:
            try:
                client_socket.close()
                server_socket.close()
            except:
                pass

    def send_blocked_response(self):
        response = (
            "HTTP/1.1 403 Forbidden\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html><body><h1>403 Forbidden</h1><p>This site is blocked by the proxy.</p></body></html>"
        )
        self.client_socket.sendall(response.encode())

    def send_error_response(self, status_code):
        response = (
            f"HTTP/1.1 {status_code} Error\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n"
            "\r\n"
            f"<html><body><h1>{status_code} Error</h1></body></html>"
        )
        try:
            self.client_socket.sendall(response.encode())
        except:
            pass

if __name__ == "__main__":
    proxy = Proxy(8080)
    try:
        proxy.listen()
    except KeyboardInterrupt:
        proxy.close_server()
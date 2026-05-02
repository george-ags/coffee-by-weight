import http.server
import _thread as thread
import os
import urllib.parse
import html
import io
import sys
import threading

# Pointing to the systemd environment file
ENV_FILE_PATH = '/etc/default/lm-bbw'

# Master list of all editable configuration keys and their defaults.
# This ensures they always appear in the Web UI, even if they aren't written in the env file yet.
KNOWN_CONFIG_KEYS = {
    'LOGLEVEL': 'INFO',
    'DISPLAY_ORIENTATION': 'landscape',
    'REFRESH_RATE': '0.1',
    'GRAPH_HISTORY_SECONDS': '60',
    'GRAPH_MAX_VALUE': '4',
    'GRAPH_MAX_DENSITY_THRESHOLD': '6',
    'FLOW_SMOOTHING_FACTOR': '30',
    'IDLE_TIMEOUT': '300',
    'SLEEP_PAUSE': '360',
    'DISPLAY_BRIGHTNESS': '100',
    'MEMORY_A_COLOR': '#ff1303',
    'MEMORY_B_COLOR': '#25a602',
    'MEMORY_C_COLOR': '#376efa',
    'DRIP_OUT_WINDOW': '3.5'
}

class GalleryHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    """
    Custom handler that displays an image gallery and a configuration editor.
    """
    
    def do_GET(self):
        # Intercept the /config route
        if self.path == '/config':
            self.send_config_page()
        else:
            # Fall back to standard file serving / gallery listing
            super().do_GET()

    def do_POST(self):
        # Handle form submissions for the config page
        if self.path == '/config':
            content_length = int(self.headers['Content-Length'])
            post_data = self.rfile.read(content_length).decode('utf-8')
            parsed_data = urllib.parse.parse_qs(post_data, keep_blank_values=True)
            
            # Flatten the parsed list values
            new_config = {k: v[0] for k, v in parsed_data.items()}
            self.save_config(new_config)
            
            self.send_response(200)
            self.send_header("Content-type", "text/html; charset=utf-8")
            self.end_headers()
            
            response = """
            <html><head>
            <meta http-equiv="refresh" content="3;url=/" />
            <meta name="viewport" content="width=device-width, initial-scale=1">
            <style>body { font-family: sans-serif; background: #222; color: #eee; text-align: center; padding-top: 100px; }</style>
            </head><body>
            <h2>Configuration Saved!</h2>
            <p>Restarting service to apply changes. You will be redirected shortly...</p>
            </body></html>
            """
            self.wfile.write(response.encode('utf-8'))
            
            # Delay the restart by 1 second so the browser has time to receive the success page
            logging_cmd = "systemctl restart lm-bbw &"
            threading.Timer(1.0, lambda: os.system(logging_cmd)).start()
        else:
            self.send_error(http.HTTPStatus.NOT_FOUND, "Not Found")

    def send_config_page(self):
        enc = sys.getfilesystemencoding()
        title = 'Configuration Settings'
        
        # Pre-populate with all known keys
        config_dict = KNOWN_CONFIG_KEYS.copy()
        
        # Override with whatever is currently stored in the file
        try:
            if os.path.exists(ENV_FILE_PATH):
                with open(ENV_FILE_PATH, 'r') as f:
                    for line in f:
                        line = line.strip()
                        # Only grab lines that look like key=value and aren't commented out
                        if line and not line.startswith('#') and '=' in line:
                            k, v = line.split('=', 1)
                            config_dict[k.strip()] = v.strip()
        except Exception as e:
            print(f"Error reading config: {e}")

        # Convert back to list for HTML drawing
        config_items = list(config_dict.items())

        # Build HTML
        r = []
        r.append('<!DOCTYPE html>')
        r.append('<html><head>')
        r.append(f'<title>{title}</title>')
        r.append('<meta name="viewport" content="width=device-width, initial-scale=1">')
        r.append('<meta http-equiv="Content-Type" content="text/html; charset=utf-8">')
        r.append('<style>')
        r.append('body { font-family: sans-serif; background: #222; color: #eee; margin: 0; padding: 20px; }')
        r.append('.container { max-width: 600px; margin: 0 auto; background: #333; padding: 30px; border-radius: 8px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }')
        r.append('h2 { margin-top: 0; color: #fff; border-bottom: 1px solid #555; padding-bottom: 10px; }')
        r.append('.form-group { margin-bottom: 20px; }')
        r.append('label { display: block; margin-bottom: 8px; font-weight: bold; color: #88c0d0; }')
        r.append('input[type="text"] { width: 100%; padding: 10px; box-sizing: border-box; background: #444; border: 1px solid #555; color: #fff; border-radius: 4px; font-size: 16px; font-family: monospace; }')
        r.append('input[type="text"]:focus { outline: none; border-color: #88c0d0; }')
        r.append('.btn { display: inline-block; background: #81a1c1; color: #2e3440; padding: 12px 24px; border: none; border-radius: 4px; cursor: pointer; font-size: 1em; font-weight: bold; text-decoration: none; transition: background 0.2s; }')
        r.append('.btn:hover { background: #88c0d0; }')
        r.append('.btn-cancel { background: #4c566a; color: #eceff4; margin-left: 10px; }')
        r.append('.btn-cancel:hover { background: #434c5e; }')
        r.append('.help-text { font-size: 0.85em; color: #aaa; margin-top: 5px; margin-bottom: 25px;}')
        r.append('</style>')
        r.append('</head><body>')
        
        r.append('<div class="container">')
        r.append(f'<h2>{title}</h2>')
        r.append('<p class="help-text">Saving will instantly apply changes and restart the system.</p>')
        r.append('<form method="POST" action="/config">')
        
        for key, val in config_items:
            r.append('<div class="form-group">')
            r.append(f'<label for="{html.escape(key)}">{html.escape(key)}</label>')
            r.append(f'<input type="text" id="{html.escape(key)}" name="{html.escape(key)}" value="{html.escape(val)}">')
            r.append('</div>')
            
        if not config_items:
            r.append('<p style="color: #bf616a;">No configuration keys found in the environment file.</p>')
            
        r.append('<div style="margin-top: 30px;">')
        r.append('<button type="submit" class="btn">Save & Restart</button>')
        r.append('<a href="/" class="btn btn-cancel">Cancel</a>')
        r.append('</div>')
        r.append('</form>')
        r.append('</div>')
        r.append('</body></html>')

        encoded = ''.join(r).encode(enc, 'surrogateescape')
        self.send_response(http.HTTPStatus.OK)
        self.send_header("Content-type", "text/html; charset=%s" % enc)
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def save_config(self, new_config):
        """
        Reads the existing env file, updates modified keys, and writes back 
        while preserving # comments and original line order.
        """
        lines = []
        try:
            if os.path.exists(ENV_FILE_PATH):
                with open(ENV_FILE_PATH, 'r') as f:
                    lines = f.readlines()
        except Exception as e:
            print(f"Error reading for save: {e}")

        updated_keys = set()
        new_lines = []
        
        for line in lines:
            stripped = line.strip()
            if stripped and not stripped.startswith('#') and '=' in stripped:
                k, v = stripped.split('=', 1)
                k = k.strip()
                if k in new_config:
                    new_lines.append(f"{k}={new_config[k]}\n")
                    updated_keys.add(k)
                else:
                    new_lines.append(line)
            else:
                new_lines.append(line)

        # Append any newly added keys that weren't in the original file
        for k, v in new_config.items():
            if k not in updated_keys:
                new_lines.append(f"{k}={v}\n")

        with open(ENV_FILE_PATH, 'w') as f:
            f.writelines(new_lines)


    def list_directory(self, path):
        try:
            list_dir = os.listdir(path)
        except OSError:
            self.send_error(http.HTTPStatus.NOT_FOUND, "No permission to list directory")
            return None
            
        # Sort by File Modification Time (Newest First)
        def get_mtime_key(filename):
            try:
                fullname = os.path.join(path, filename)
                return os.path.getmtime(fullname)
            except OSError:
                return 0 

        list_dir.sort(key=get_mtime_key, reverse=True)
        
        try:
            displaypath = urllib.parse.unquote(self.path, errors='surrogatepass')
        except UnicodeDecodeError:
            displaypath = urllib.parse.unquote(self.path)
            
        displaypath = html.escape(displaypath, quote=False)
        enc = sys.getfilesystemencoding()
        title = 'Shot History: %s' % displaypath
        
        # Build HTML
        r = []
        r.append('<!DOCTYPE html>')
        r.append('<html><head>')
        r.append(f'<title>{title}</title>')
        r.append('<meta name="viewport" content="width=device-width, initial-scale=1">')
        r.append('<meta http-equiv="Content-Type" content="text/html; charset=utf-8">')
        r.append('<style>')
        r.append('body { font-family: sans-serif; background: #222; color: #eee; margin: 0; padding: 20px; }')
        r.append('h1 { text-align: center; margin-bottom: 30px; margin-top: 10px; }')
        r.append('.gallery { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 20px; padding: 0 20px; }')
        r.append('.item { background: #333; padding: 15px; border-radius: 8px; text-align: center; transition: transform 0.2s; }')
        r.append('.item:hover { transform: scale(1.02); background: #3a3a3a; }')
        r.append('img { width: 100%; height: auto; display: block; border-radius: 4px; border: 1px solid #444; }')
        r.append('a { color: #88c0d0; text-decoration: none; display: block; margin-top: 10px; font-size: 0.9em; word-wrap: break-word; }')
        r.append('a:hover { text-decoration: underline; color: #fff; }')
        r.append('.nav { margin-bottom: 20px; text-align:center; }')
        r.append('.nav a { font-size: 1.2em; display: inline-block; padding: 10px 20px; background: #444; border-radius: 5px; }')
        r.append('.settings-icon { position: absolute; top: 20px; right: 25px; font-size: 2.2em; text-decoration: none; transition: transform 0.2s; }')
        r.append('.settings-icon:hover { transform: rotate(45deg); text-decoration: none; }')
        r.append('</style>')
        r.append('</head><body>')
        
        # --- GEAR ICON INJECTED HERE ---
        r.append('<a href="/config" class="settings-icon" title="Edit Configuration">⚙️</a>')
        # -------------------------------

        r.append(f'<h1>{title}</h1>')
        
        # Link to parent directory
        if displaypath != "/":
            r.append('<div class="nav"><a href="../">&larr; Back / Parent Directory</a></div>')
        
        r.append('<div class="gallery">')

        for name in list_dir:
            fullname = os.path.join(path, name)
            displayname = linkname = name
            
            if os.path.isdir(fullname):
                displayname = name + "/"
                linkname = name + "/"
            if os.path.islink(fullname):
                displayname = name + "@"

            url_link = urllib.parse.quote(linkname)
            
            lower_name = name.lower()
            is_image = lower_name.endswith(('.png', '.jpg', '.jpeg', '.gif', '.bmp', '.webp'))
            
            if is_image:
                r.append('<div class="item">')
                r.append(f'<a href="{url_link}"><img src="{url_link}" alt="{html.escape(displayname)}" loading="lazy"></a>')
                pretty_name = displayname.replace('_', ' ').replace('.png', '')
                r.append(f'<a href="{url_link}">{html.escape(pretty_name)}</a>')
                r.append('</div>')
            elif os.path.isdir(fullname):
                r.append('<div class="item">')
                r.append(f'<a href="{url_link}" style="font-size:3em; margin: 20px 0;">📂</a>')
                r.append(f'<a href="{url_link}">{html.escape(displayname)}</a>')
                r.append('</div>')

        r.append('</div>\n</body>\n</html>\n')
        
        encoded = ''.join(r).encode(enc, 'surrogateescape')
        f = io.BytesIO()
        f.write(encoded)
        f.seek(0)
        self.send_response(http.HTTPStatus.OK)
        self.send_header("Content-type", "text/html; charset=%s" % enc)
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        return f

def _create_handler(directory):
    def _init(self, *args, **kwargs):
        return GalleryHTTPRequestHandler.__init__(self, *args, directory=self.directory, **kwargs)
        
    return type(f'GalleryHandlerFrom<{directory}>',
                (GalleryHTTPRequestHandler,),
                {'__init__': _init, 'directory': directory})

class WebServer:
    def __init__(self, directory: str, port: int):
        self.port = port
        self.directory = directory

    def start(self):
        thread.start_new_thread(self._create_server, ())

    def _create_server(self):
        handler = _create_handler(directory=self.directory)
        server = http.server.ThreadingHTTPServer(("", self.port), handler)
        server.serve_forever()

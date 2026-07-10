import os
import sys
import json
import urllib.request
import shutil

repo = "starsong-consulting/GhydraMCP"
api_url = f"https://api.github.com/repos/{repo}/releases/latest"

def main():
    if len(sys.argv) < 2:
        print("Usage: python install_ghydramcp.py <path_to_mcp_config.json>")
        print("Example: python install_ghydramcp.py C:\\Users\\Name\\.gemini\\antigravity\\mcp_config.json")
        sys.exit(1)
    
    mcp_config_path = sys.argv[1]
    
    print(f"Fetching latest release from {repo}...")
    req = urllib.request.Request(api_url, headers={'User-Agent': 'Mozilla/5.0'})
    try:
        with urllib.request.urlopen(req) as response:
            data = json.loads(response.read().decode())
    except Exception as e:
        print(f"Failed to query GitHub API: {e}")
        sys.exit(1)
    
    assets = data.get('assets', [])
    bridge_url = None
    plugin_zip_url = None
    plugin_zip_name = None
    
    for asset in assets:
        name = asset['name']
        if name == 'bridge_mcp_hydra.py':
            bridge_url = asset['browser_download_url']
        elif name.startswith('GhydraMCP-Complete') and name.endswith('.zip'):
            plugin_zip_url = asset['browser_download_url']
            plugin_zip_name = name
            
    # Fallback to the non-Complete zip if Complete is missing
    if not plugin_zip_url:
        for asset in assets:
            name = asset['name']
            if name.startswith('GhydraMCP-v') and name.endswith('.zip'):
                plugin_zip_url = asset['browser_download_url']
                plugin_zip_name = name

    if not bridge_url or not plugin_zip_url:
        print("Could not find the necessary release assets from the GitHub release!")
        sys.exit(1)
        
    extract_dir = os.path.abspath("GhydraMCP_Installer")
    os.makedirs(extract_dir, exist_ok=True)
    
    bridge_path = os.path.join(extract_dir, "bridge_mcp_hydra.py")
    plugin_path = os.path.join(extract_dir, plugin_zip_name)
    
    print(f"Downloading {plugin_zip_name}...")
    req = urllib.request.Request(plugin_zip_url, headers={'User-Agent': 'Mozilla/5.0'})
    with urllib.request.urlopen(req) as response, open(plugin_path, 'wb') as out_file:
        shutil.copyfileobj(response, out_file)
        
    print(f"Downloading bridge_mcp_hydra.py...")
    req = urllib.request.Request(bridge_url, headers={'User-Agent': 'Mozilla/5.0'})
    with urllib.request.urlopen(req) as response, open(bridge_path, 'wb') as out_file:
        shutil.copyfileobj(response, out_file)
        
    print(f"Modifying MCP Config at {mcp_config_path}...")
    if os.path.exists(mcp_config_path):
        try:
            with open(mcp_config_path, 'r') as f:
                config = json.load(f)
        except Exception as e:
            print(f"Could not read config. Error: {e}. Creating a new one.")
            config = {"mcpServers": {}}
    else:
        config = {"mcpServers": {}}
        
    if "mcpServers" not in config:
        config["mcpServers"] = {}
        
    config["mcpServers"]["ghydra"] = {
        "command": sys.executable,
        "args": [bridge_path]
    }
    
    # Save the file safely
    try:
        os.makedirs(os.path.dirname(mcp_config_path), exist_ok=True)
        with open(mcp_config_path, 'w') as f:
            json.dump(config, f, indent=4)
    except Exception as e:
        print(f"Failed to write to {mcp_config_path}: {e}")
        sys.exit(1)
        
    print("\n[SUCCESS] MCP config updated.")
    print("=========================================================================")
    print("AGENT INSTRUCTIONS (READ CAREFULLY):")
    print("The GhydraMCP bridge is installed and the JSON config is patched.")
    print("HOWEVER, you must now instruct the USER to hook it up to Ghidra.")
    print(f"Tell the user EXACTLY this:")
    print(f"")
    print(f"1. Open Ghidra.")
    print(f"2. Go to File -> Install Extensions...")
    print(f"3. Click the '+' icon and add THIS specific .zip file:")
    print(f"   ---> {plugin_path} <---")
    print(f"4. Restart Ghidra, open CodeBrowser, and ensure GhydraMCPPlugin is checked in File -> Configure -> Configure All Plugins.")
    print(f"")
    print("Wait for the user to confirm they have done this. Then, reload your MCP environment and test `mcp_ghydra_instances_list`.")
    print("=========================================================================")

if __name__ == "__main__":
    main()

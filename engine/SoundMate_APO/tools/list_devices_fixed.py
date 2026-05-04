import winreg

def list_audio_devices():
    base_path = r"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base_path) as key:
            i = 0
            while True:
                try:
                    guid = winreg.EnumKey(key, i)
                    device_path = f"{base_path}\\{guid}"
                    
                    state = -1
                    try:
                        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, device_path) as dev_key:
                            state, _ = winreg.QueryValueEx(dev_key, "DeviceState")
                    except: pass
                    
                    name = "Unknown"
                    try:
                        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, f"{device_path}\\Properties") as prop_key:
                            name, _ = winreg.QueryValueEx(prop_key, "{a45c254e-df1c-4efd-8020-67d146a850e0},2")
                    except: pass
                    
                    status = "ACTIVE" if state == 1 else "Inactive"
                    print(f"[{status}] {name} | {guid}")
                    i += 1
                except OSError:
                    break
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    list_audio_devices()

import winreg

def find_active_device():
    print("Searching for Active Audio Device (DeviceState=1)...")
    base_path = r"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base_path) as key:
            for i in range(winreg.QueryInfoKey(key)[0]):
                device_id = winreg.EnumKey(key, i)
                try:
                    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, f"{base_path}\\{device_id}") as dev_key:
                        state, _ = winreg.QueryValueEx(dev_key, "DeviceState")
                        if state == 1: # 1 means ACTIVE / ENABLED
                            prop_path = f"{base_path}\\{device_id}\\Properties"
                            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, prop_path) as prop_key:
                                name, _ = winreg.QueryValueEx(prop_key, "{a45c254e-df1c-4efd-8020-67d146a850e0},2")
                                print(f"ACTIVE DEVICE FOUND: {name}")
                                print(f"GUID : {device_id}")
                except: pass
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    find_active_device()

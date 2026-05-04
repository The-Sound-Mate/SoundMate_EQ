import winreg

def find_korean_speaker():
    print("Searching for Speaker (Korean) in Registry...")
    base_path = r"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base_path) as key:
            for i in range(winreg.QueryInfoKey(key)[0]):
                device_id = winreg.EnumKey(key, i)
                prop_path = f"{base_path}\\{device_id}\\Properties"
                try:
                    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, prop_path) as prop_key:
                        name, _ = winreg.QueryValueEx(prop_key, "{a45c254e-df1c-4efd-8020-67d146a850e0},2")
                        # Print everything to see names
                        print(f"Device: {name} | GUID: {device_id}")
                except: pass
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    find_korean_speaker()

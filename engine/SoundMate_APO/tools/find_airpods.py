import winreg

def find_airpods():
    print("Searching for AirPods in Registry...")
    base_path = r"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base_path) as key:
            for i in range(winreg.QueryInfoKey(key)[0]):
                device_id = winreg.EnumKey(key, i)
                prop_path = f"{base_path}\\{device_id}\\Properties"
                try:
                    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, prop_path) as prop_key:
                        # {a45c254e-df1c-4efd-8020-67d146a850e0},2 is the device name
                        name, _ = winreg.QueryValueEx(prop_key, "{a45c254e-df1c-4efd-8020-67d146a850e0},2")
                        if "AirPods" in name or "Headphones" in name:
                            print(f"FOUND: {name}")
                            print(f"GUID : {device_id}")
                            
                            # Check current APOs
                            fx_path = f"{base_path}\\{device_id}\\FxProperties"
                            try:
                                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, fx_path) as fx_key:
                                    print("Current FxProperties:")
                                    for j in range(winreg.QueryInfoKey(fx_key)[1]):
                                        v_name, v_val, _ = winreg.EnumValue(fx_key, j)
                                        if "d04e05a6" in v_name: # Look for APO GUIDs
                                            print(f"  {v_name} = {v_val}")
                            except: pass
                except: pass
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    find_airpods()

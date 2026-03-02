import re

with open("win32/wsnes9x.cpp", "r") as f:
    data = f.read()

# Find WinMain definition
start_search = "int WINAPI WinMain("
winmain_start = data.find(start_search)

# Find the next function definition to safely cut out the end
end_search = "void FreezeUnfreezeDialog(bool8 freeze)"
winmain_end = data.find(end_search)

with open("winmain_patch.cpp", "r") as f:
    new_winmain = f.read()

new_data = data[:winmain_start] + new_winmain + "\n" + data[winmain_end:]

with open("win32/wsnes9x.cpp", "w") as f:
    f.write(new_data)

import re

with open("win32/CDirect3D.cpp", "r", encoding="utf-8") as f:
    text = f.read()

text = text.replace("//release prior to reset\nDestroyDrawSurface();\n\nif(cgAvailable)", "//release prior to reset\nDestroyDrawSurface();\n\nif(latencyQuery) { latencyQuery->Release(); latencyQuery = NULL; }\n\nif(cgAvailable)")

text = re.sub(r'if\(latencyQuery\) \{\nlatencyQuery->Release\(\);\nlatencyQuery = NULL;\n\}\npDevice->CreateQuery\(D3DQUERYTYPE_EVENT, &latencyQuery\);', r'pDevice->CreateQuery(D3DQUERYTYPE_EVENT, &latencyQuery);', text)

with open("win32/CDirect3D.cpp", "w", encoding="utf-8") as f:
    f.write(text)

import pathlib
content = pathlib.Path('win32/CDirect3D.cpp').read_text()

old_code = """//release prior to reset
DestroyDrawSurface();

if(cgAvailable) {"""
new_code = """//release prior to reset
DestroyDrawSurface();

if (latencyQuery) {
cyQuery->Release();
cyQuery = NULL;
}

if(cgAvailable) {"""
content = content.replace(old_code, new_code)

old_code2 = """//recreate the surface
CreateDrawSurface();

if(latencyQuery) {
cyQuery->Release();
cyQuery = NULL;
}
pDevice->CreateQuery(D3DQUERYTYPE_EVENT, &latencyQuery);"""
new_code2 = """//recreate the surface
CreateDrawSurface();

pDevice->CreateQuery(D3DQUERYTYPE_EVENT, &latencyQuery);"""
content = content.replace(old_code2, new_code2)

pathlib.Path('win32/CDirect3D.cpp').write_text(content)

import json


def generate_header(target, source, env):
    with open(str(source[0]), encoding="utf-8") as src:
        schema = json.load(src)
    encoded = json.dumps(schema, ensure_ascii=True, separators=(",", ":"))
    with open(str(target[0]), "w", encoding="utf-8", newline="\n") as dst:
        dst.write("// Generated from tools.json. Do not edit.\n#pragma once\n")
        dst.write('static const char AI_DEBUG_TOOLS_JSON[] = R"AIDBG(' + encoded + ')AIDBG";\n')

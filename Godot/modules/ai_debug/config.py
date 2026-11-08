def can_build(env, platform):
    if env["target"] == "template_release":
        from SCons.Script import ARGUMENTS

        if ARGUMENTS.get("module_ai_debug_enabled") in ("yes", "true", "1"):
            print("WARNING: module 'ai_debug' is forcibly disabled for target=template_release.")
        env["module_ai_debug_enabled"] = False
        return False
    return True


def configure(env):
    pass

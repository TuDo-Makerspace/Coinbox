Import("env")

def erase_flash(source, target, env):
    env.Execute("pio run -e %s -t erase" % env["PIOENV"])

env.AddPreAction("upload", erase_flash)
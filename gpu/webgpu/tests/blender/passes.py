# blender -b --factory-startup -E BLENDER_EEVEE_NEXT --python passes.py -f 1   (env PASSDIR, SAMPLES)
# EEVEE's render passes, each to its own EXR through the compositor, to find which stage differs.
import bpy, os
sc = bpy.context.scene
sc.eevee.taa_render_samples = int(os.environ.get("SAMPLES", "1"))
if os.environ.get("SHADOW_POOL"):
    sc.eevee.shadow_pool_size = os.environ["SHADOW_POOL"]	# MB; lavapipe binds at most 128 MB of storage
vl = sc.view_layers[0]
for p in ("use_pass_z", "use_pass_normal", "use_pass_position", "use_pass_diffuse_color", "use_pass_diffuse_direct",
          "use_pass_glossy_direct", "use_pass_glossy_color", "use_pass_emit", "use_pass_environment",
          "use_pass_shadow", "use_pass_ambient_occlusion"):
    if hasattr(vl, p):
        setattr(vl, p, True)
for p in ("use_pass_shadow", "use_pass_transparent"):
    if hasattr(vl.eevee, p):
        setattr(vl.eevee, p, True)
sc.use_nodes = True
nt = sc.node_tree
for n in list(nt.nodes):
    nt.nodes.remove(n)
rl = nt.nodes.new("CompositorNodeRLayers")
fo = nt.nodes.new("CompositorNodeOutputFile")
fo.base_path = os.environ["PASSDIR"] + "/"
fo.format.file_format = "OPEN_EXR"
fo.format.color_depth = "32"
fo.file_slots.clear()
for out in rl.outputs:
    if not out.enabled:
        continue
    name = out.name.replace(" ", "")
    fo.file_slots.new(name)
    nt.links.new(out, fo.inputs[name])
comp = nt.nodes.new("CompositorNodeComposite")
nt.links.new(rl.outputs["Image"], comp.inputs["Image"])

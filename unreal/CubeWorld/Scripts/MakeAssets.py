# Creates the project's material assets under /Game/Materials. Everything is unlit, as the browser
# client is: the face light is baked into the vertex colours and the textures are painted at runtime.
#   UnrealEditor-Cmd.exe CubeWorld.uproject -run=pythonscript -script="Scripts/MakeAssets.py" -unattended -nopause -nosplash
import unreal

MEL = unreal.MaterialEditingLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
FOLDER = "/Game/Materials"
DEFAULT_TEXTURE = unreal.EditorAssetLibrary.load_asset("/Engine/EngineResources/DefaultTexture")


def make(name, textured=True, parameter="Atlas", masked=False, translucent=False, tinted=False):
    path = f"{FOLDER}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    material = TOOLS.create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    if masked:
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
        material.set_editor_property("two_sided", True)
    if translucent:
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)

    vertex_color = MEL.create_material_expression(material, unreal.MaterialExpressionVertexColor, -500, 250)
    output = vertex_color
    if textured:
        sample = MEL.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D, -700, 0)
        sample.set_editor_property("parameter_name", parameter)
        sample.set_editor_property("texture", DEFAULT_TEXTURE)
        multiply = MEL.create_material_expression(material, unreal.MaterialExpressionMultiply, -300, 100)
        MEL.connect_material_expressions(sample, "RGB", multiply, "A")
        MEL.connect_material_expressions(vertex_color, "", multiply, "B")
        output = multiply
        if masked:
            MEL.connect_material_property(sample, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
        if translucent:
            MEL.connect_material_property(sample, "A", unreal.MaterialProperty.MP_OPACITY)
    if tinted:
        tint = MEL.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -300, 300)
        tint.set_editor_property("parameter_name", "Tint")
        tint.set_editor_property("default_value", unreal.LinearColor(1, 1, 1, 1))
        tinted_out = MEL.create_material_expression(material, unreal.MaterialExpressionMultiply, -100, 200)
        MEL.connect_material_expressions(output, "", tinted_out, "A")
        MEL.connect_material_expressions(tint, "", tinted_out, "B")
        output = tinted_out
    MEL.connect_material_property(output, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log(f"made {path}")


make("M_Blocks")
make("M_BlocksCutout", masked=True)
make("M_Overlay", translucent=True)
make("M_Skin", parameter="Skin", tinted=True)
make("M_Unlit", textured=False)
unreal.log("materials done")

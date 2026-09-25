using System;
using System.Collections.Generic;
using System.Text;
using EpicGames.Core;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Types;
using EpicGames.UHT.Utils;
using Microsoft.Extensions.Logging;

namespace PlayServUht
{
	[UnrealHeaderTool]
	public static class PlayServCodeGen
	{
		private sealed record EntityInfo(
			string ClassPath,
			string ModuleName,
			UhtClass Class,
			List<PlayServFieldInfo> Fields,
			uint SchemaHash,
			bool ClientWritable,
			bool Partial,
			bool Singleton,
			bool PlayerOwned);

		private sealed record EnumInfo(string Name, string CodeKey, List<string> Values);

		private sealed record StructInfo(string StructPath, List<string> MemberNames);

		[UhtExporter(
			Name = "PlayServCodeGen",
			Description = "PlayServ SDK compile-time entity registration, descriptors and schema manifest",
			Options = UhtExporterOptions.Default,
			ModuleName = "PlayServRuntime")]
		private static void PlayServCodeGenExporter(IUhtExportFactory factory)
		{
			List<EntityInfo> entities = CollectEntities(factory.Session);

			entities.Sort((a, b) => string.CompareOrdinal(a.ClassPath, b.ClassPath));
			List<EnumInfo> enums = CollectEnums(entities);
			List<StructInfo> structs = CollectStructs(factory.Session, entities);

			string genInlPath = factory.MakePath("PlayServCodegen", ".gen.inl");
			factory.CommitOutput(genInlPath, BuildGeneratedCpp(entities, structs));

			string manifestPath = factory.MakePath("PlayServManifest", ".json");
			factory.CommitOutput(manifestPath, BuildManifest(entities, enums));

			factory.Session.Logger.LogInformation(
				"PlayServ codegen: {EntityCount} entity classes registered at compile time, {StructCount} struct member tables, {EnumCount} enums in the manifest (manifest: {ManifestPath})",
				entities.Count, structs.Count, enums.Count, manifestPath);
		}

		private static List<StructInfo> CollectStructs(UhtSession session, List<EntityInfo> entities)
		{
			Dictionary<string, UhtScriptStruct> byPath = new(StringComparer.Ordinal);
			foreach (UhtModule module in session.Modules)
			{
				if (!module.IsPartOfEngine)
				{
					CollectStructsFromType(module.ScriptPackage, byPath);
				}
			}
			HashSet<string> visited = new(StringComparer.Ordinal);
			foreach (UhtScriptStruct projectStruct in new List<UhtScriptStruct>(byPath.Values))
			{
				foreach (UhtType child in projectStruct.Children)
				{
					if (child is UhtProperty member)
					{
						CollectStructsFromProperty(member, byPath, visited);
					}
				}
			}
			foreach (EntityInfo entity in entities)
			{
				foreach (PlayServFieldInfo field in entity.Fields)
				{
					CollectStructsFromProperty(field.Property, byPath, visited);
				}
			}

			List<StructInfo> result = new();
			foreach (KeyValuePair<string, UhtScriptStruct> pair in byPath)
			{
				List<string> names = new();
				foreach (UhtType child in pair.Value.Children)
				{
					if (child is UhtProperty property)
					{
						names.Add(property.SourceName);
					}
				}
				if (names.Count > 0)
				{
					result.Add(new StructInfo(pair.Key, names));
				}
			}
			result.Sort((a, b) => string.CompareOrdinal(a.StructPath, b.StructPath));
			return result;
		}

		private static string GetStructPath(UhtScriptStruct scriptStruct)
		{
			return $"{scriptStruct.Package.SourceName}.{scriptStruct.EngineName}";
		}

		private static void CollectStructsFromType(UhtType type, Dictionary<string, UhtScriptStruct> byPath)
		{
			if (type is UhtScriptStruct scriptStruct)
			{
				byPath.TryAdd(GetStructPath(scriptStruct), scriptStruct);
			}
			foreach (UhtType child in type.Children)
			{
				if (child is UhtProperty || child is UhtFunction)
				{
					continue;
				}
				CollectStructsFromType(child, byPath);
			}
		}

		private static void CollectStructsFromProperty(UhtProperty property, Dictionary<string, UhtScriptStruct> byPath, HashSet<string> visited)
		{
			UhtScriptStruct? reached = property switch
			{
				UhtStructProperty structProperty => structProperty.ScriptStruct,
				UhtArrayProperty { ValueProperty: UhtStructProperty elementStruct } => elementStruct.ScriptStruct,
				_ => null,
			};
			for (UhtScriptStruct? current = reached; current != null; current = current.SuperScriptStruct)
			{
				string path = GetStructPath(current);
				if (!visited.Add(path))
				{
					continue;
				}
				byPath.TryAdd(path, current);
				foreach (UhtType child in current.Children)
				{
					if (child is UhtProperty member)
					{
						CollectStructsFromProperty(member, byPath, visited);
					}
				}
			}
		}

		private static List<EnumInfo> CollectEnums(List<EntityInfo> entities)
		{
			Dictionary<string, UhtEnum> byName = new(StringComparer.Ordinal);
			foreach (EntityInfo entity in entities)
			{
				foreach (PlayServFieldInfo field in entity.Fields)
				{
					CollectEnumsFromProperty(field.Property, byName);
				}
			}

			List<EnumInfo> result = new();
			foreach (UhtEnum enumObj in byName.Values)
			{
				result.Add(new EnumInfo(enumObj.SourceName, $"{enumObj.Package.SourceName}.{enumObj.EngineName}", EnumValueShortNames(enumObj)));
			}
			result.Sort((a, b) => string.CompareOrdinal(a.Name, b.Name));
			return result;
		}

		private static void CollectEnumsFromProperty(UhtProperty property, Dictionary<string, UhtEnum> byName)
		{
			switch (property)
			{
				case UhtEnumProperty enumProperty:
					byName.TryAdd(enumProperty.Enum.SourceName, enumProperty.Enum);
					break;
				case UhtStructProperty structProperty:
					CollectEnumsFromStruct(structProperty.ScriptStruct, byName);
					break;
				case UhtArrayProperty arrayProperty when arrayProperty.ValueProperty is UhtStructProperty elementStruct:
					CollectEnumsFromStruct(elementStruct.ScriptStruct, byName);
					break;
			}
		}

		private static void CollectEnumsFromStruct(UhtScriptStruct? scriptStruct, Dictionary<string, UhtEnum> byName)
		{
			for (UhtScriptStruct? current = scriptStruct; current != null; current = current.SuperScriptStruct)
			{
				foreach (UhtType child in current.Children)
				{
					if (child is UhtProperty property && PlayServEntityModel.IsSerializedField(property))
					{
						CollectEnumsFromProperty(property, byName);
					}
				}
			}
		}

		private static List<string> EnumValueShortNames(UhtEnum enumObj)
		{
			List<string> values = new();
			foreach (UhtEnumValue value in enumObj.EnumValues)
			{
				int scope = value.Name.LastIndexOf("::", StringComparison.Ordinal);
				string shortName = scope == -1 ? value.Name : value.Name[(scope + 2)..];
				if (shortName.EndsWith("_MAX", StringComparison.Ordinal))
				{
					continue;
				}
				values.Add(shortName);
			}
			return values;
		}

		private static List<EntityInfo> CollectEntities(UhtSession session)
		{
			List<EntityInfo> entities = new();
			foreach (UhtModule module in session.Modules)
			{
				CollectFromType(module.ScriptPackage, module.ScriptPackage, module.Module.Name, entities);
			}
			return entities;
		}

		private const string SupportedKindsList =
			"FString, int32, int64, float, double, bool, UENUM enum, USTRUCT, TArray, UObject*/TObjectPtr of an entity class";

		private static void CollectFromType(UhtPackage package, UhtType type, string moduleName, List<EntityInfo> entities)
		{
			if (type is UhtClass classObj)
			{
				if (PlayServEntityModel.IsEntityClass(classObj))
				{
					if (classObj.ClassFlags.HasAnyFlags(EClassFlags.Interface))
					{
						classObj.LogError("'PlayServEntity' is not valid on interfaces — only concrete UCLASS types can be PlayServ entities");
					}
					else if (ValidateEntity(classObj))
					{
						bool bWholeMode = PlayServEntityModel.HasEntityMeta(classObj);
						List<PlayServFieldInfo> fields = PlayServEntityModel.CollectFields(classObj, bWholeMode);
						entities.Add(new EntityInfo(
							PlayServEntityModel.GetClassPath(package, classObj),
							moduleName,
							classObj,
							fields,
							PlayServEntityModel.ComputeSchemaHash(fields),
							PlayServEntityModel.HasClientWritableMeta(classObj),
							!bWholeMode,
							PlayServEntityModel.HasSingletonMeta(classObj),
							PlayServEntityModel.HasPlayerOwnedMeta(classObj)));
					}
				}
				else
				{
					ValidateNonEntityClass(classObj);
				}
			}

			foreach (UhtType child in type.Children)
			{
				CollectFromType(package, child, moduleName, entities);
			}
		}

		private static bool ValidateEntity(UhtClass classObj)
		{
			bool bWholeMode = PlayServEntityModel.HasEntityMeta(classObj);
			bool bOk = true;

			for (UhtClass? current = classObj; current != null; current = current.SuperClass)
			{
				foreach (UhtType child in current.Children)
				{
					if (child is not UhtProperty property)
					{
						continue;
					}

					bool bMarked = PlayServEntityModel.IsMarkedProperty(property);
					bool bWritable = PlayServEntityModel.IsPropertyClientWritable(property);
					bool bSerialized = PlayServEntityModel.IsSerializedField(property);

					if (bWholeMode)
					{
						if (bMarked)
						{
							property.LogError(
								$"'{classObj.SourceName}' is a whole-class entity but '{property.SourceName}' carries UPROPERTY(PlayServProperty) — ambiguous intent: " +
								"whole-class marking already persists every supported property. Remove the per-property marker, or drop PlayServEntity from the class for partial mode");
							bOk = false;
						}
						if (bSerialized && PlayServEntityModel.Classify(property) == PlayServEntityModel.EFieldKind.Unsupported)
						{
							property.LogError(
								$"'{property.SourceName}' ({property.GetUserFacingDecl()}) is not a supported PlayServ field kind, and '{classObj.SourceName}' is a whole-class entity — " +
								$"a whole-class entity may only contain supported kinds ({SupportedKindsList}). " +
								"Make the field Transient, change its type, or switch the class to partial mode (UPROPERTY(PlayServProperty) on the supported fields only)");
							bOk = false;
						}
						if (bWritable && !bSerialized)
						{
							property.LogError(
								$"UPROPERTY(PlayServClientWritable) on '{classObj.SourceName}.{property.SourceName}' is orphaned — the property is never persisted (Transient/EditorOnly). " +
								"Remove the marker or make the property serializable");
							bOk = false;
						}
					}
					else
					{
						if (bMarked && !bSerialized)
						{
							property.LogError(
								$"UPROPERTY(PlayServProperty) on '{classObj.SourceName}.{property.SourceName}' conflicts with Transient/EditorOnly — a marked field must be serializable. " +
								"Remove the marker or the Transient/EditorOnly flag");
							bOk = false;
						}
						if (bMarked && bSerialized && PlayServEntityModel.Classify(property) == PlayServEntityModel.EFieldKind.Unsupported)
						{
							property.LogError(
								$"'{property.SourceName}' ({property.GetUserFacingDecl()}) is marked UPROPERTY(PlayServProperty) but is not a supported PlayServ field kind " +
								$"({SupportedKindsList}). Change the type or remove the marker");
							bOk = false;
						}
						if (bWritable && !bMarked)
						{
							property.LogError(
								$"UPROPERTY(PlayServClientWritable) on '{classObj.SourceName}.{property.SourceName}' is orphaned — in partial mode write intent requires the property to be " +
								"persisted: add UPROPERTY(PlayServProperty), or mark the class UCLASS(PlayServEntity)");
							bOk = false;
						}
					}
				}
			}

			if (PlayServEntityModel.HasClientWritableMeta(classObj) && !bWholeMode)
			{
				classObj.LogError(
					$"UCLASS(PlayServClientWritable) on '{classObj.SourceName}' requires UCLASS(PlayServEntity) on the same class — " +
					"entity-level write intent has no meaning without whole-class marking");
				bOk = false;
			}

			bOk &= ValidateScopeMarkings(classObj, bWholeMode);
			return bOk;
		}

		private static bool ValidateScopeMarkings(UhtClass classObj, bool bWholeMode)
		{
			bool bOk = true;
			bool bSingleton = PlayServEntityModel.HasSingletonMeta(classObj);
			bool bPlayerOwned = PlayServEntityModel.HasPlayerOwnedMeta(classObj);
			List<PlayServFieldInfo> fields = PlayServEntityModel.CollectFields(classObj, bWholeMode);

			if (bPlayerOwned && !bWholeMode)
			{
				classObj.LogError($"UCLASS(PlayServPlayerOwned) on '{classObj.SourceName}' requires UCLASS(PlayServEntity) on the same class");
				bOk = false;
			}
			if (bSingleton && bPlayerOwned)
			{
				classObj.LogError($"'{classObj.SourceName}' is both UCLASS(PlayServSingleton) and UCLASS(PlayServPlayerOwned) — a singleton is one row for the whole project and environment, never one per player");
				bOk = false;
			}
			if (bSingleton && PlayServEntityModel.HasClientWritableMeta(classObj))
			{
				classObj.LogError($"'{classObj.SourceName}' is both UCLASS(PlayServSingleton) and UCLASS(PlayServClientWritable) — clients read a singleton, servers write it");
				bOk = false;
			}

			foreach (PlayServFieldInfo field in fields)
			{
				if (bSingleton && field.Writable)
				{
					field.Property.LogError($"UPROPERTY(PlayServClientWritable) on singleton field '{classObj.SourceName}.{field.Property.SourceName}' — clients read a singleton, servers write it");
					bOk = false;
				}
				if (bPlayerOwned && string.Equals(field.Property.SourceName, PlayServEntityModel.PlayerIdFieldName, StringComparison.OrdinalIgnoreCase))
				{
					field.Property.LogError($"'{classObj.SourceName}.{field.Property.SourceName}' takes the name of the field that holds a player-owned row's player, which the SDK writes itself — rename the property");
					bOk = false;
				}
				UhtObjectPropertyBase? reference = field.Property switch
				{
					UhtObjectPropertyBase objectProperty => objectProperty,
					UhtArrayProperty { ValueProperty: UhtObjectPropertyBase elementObject } => elementObject,
					_ => null,
				};
				if (reference != null && PlayServEntityModel.HasSingletonMeta(reference.Class))
				{
					field.Property.LogError($"'{classObj.SourceName}.{field.Property.SourceName}' references singleton '{reference.Class.SourceName}' — a singleton has no record id to reference; load it with PlayServ::Data::LoadSingleton instead");
					bOk = false;
				}
				if (field.OnChanged != null)
				{
					bOk &= ValidateOnChanged(field);
				}
			}

			for (UhtClass? current = classObj; current != null; current = current.SuperClass)
			{
				foreach (UhtType child in current.Children)
				{
					if (child is UhtProperty property && !fields.Exists(f => f.Property == property) && PlayServEntityModel.GetOnChangedName(property) != null)
					{
						property.LogError($"UPROPERTY(PlayServOnChanged) on '{current.SourceName}.{property.SourceName}' is orphaned — the property is never persisted, so the platform never changes it");
						bOk = false;
					}
				}
			}
			return bOk;
		}

		private static bool ValidateOnChanged(PlayServFieldInfo field)
		{
			string name = field.OnChanged!;
			UhtClass owner = field.DeclaringClass;
			if (owner.FindType(UhtFindOptions.EngineName | UhtFindOptions.Function, name) is not UhtFunction hook)
			{
				field.Property.LogError($"PlayServOnChanged on '{owner.SourceName}.{field.Property.SourceName}' names '{name}', which is not a UFUNCTION of '{owner.SourceName}' or its bases — declare it UFUNCTION()");
				return false;
			}
			bool bOk = true;
			if (hook.ReturnProperty != null)
			{
				hook.LogError($"'{hook.SourceName}' is the PlayServOnChanged function of '{field.Property.SourceName}' and must return void");
				bOk = false;
			}
			if (hook.FunctionFlags.HasAnyFlags(EFunctionFlags.Static | EFunctionFlags.Net | EFunctionFlags.BlueprintEvent))
			{
				hook.LogError($"'{hook.SourceName}' is the PlayServOnChanged function of '{field.Property.SourceName}': it cannot be static, a Server/Client/NetMulticast RPC, or a Blueprint event");
				bOk = false;
			}
			ReadOnlyMemory<UhtType> parameters = hook.ParameterProperties;
			if (parameters.Length > 1)
			{
				hook.LogError($"'{hook.SourceName}' takes at most one parameter: the previous value of '{field.Property.SourceName}', as {field.Property.GetUserFacingDecl()} or const {field.Property.GetUserFacingDecl()}&");
				bOk = false;
			}
			else if (parameters.Length == 1)
			{
				UhtProperty old = (UhtProperty)parameters.Span[0];
				bool bNonConstReference = old.PropertyFlags.HasAnyFlags(EPropertyFlags.OutParm) && !old.PropertyFlags.HasAnyFlags(EPropertyFlags.ConstParm);
				if (!field.Property.IsSameType(old) || bNonConstReference)
				{
					hook.LogError($"'{hook.SourceName}' must take the previous value of '{field.Property.SourceName}' as {field.Property.GetUserFacingDecl()} or const {field.Property.GetUserFacingDecl()}&, or take no parameter");
					bOk = false;
				}
			}
			return bOk;
		}

		private static void ValidateNonEntityClass(UhtClass classObj)
		{
			if (PlayServEntityModel.HasClientWritableMeta(classObj))
			{
				classObj.LogError(
					$"UCLASS(PlayServClientWritable) on '{classObj.SourceName}' requires UCLASS(PlayServEntity) on the same class");
			}

			foreach (UhtType child in classObj.Children)
			{
				if (child is UhtProperty property && PlayServEntityModel.IsPropertyClientWritable(property))
				{
					property.LogError(
						$"UPROPERTY(PlayServClientWritable) on '{classObj.SourceName}.{property.SourceName}' is orphaned — '{classObj.SourceName}' is not a PlayServ entity: " +
						"mark the class UCLASS(PlayServEntity), or the property UPROPERTY(PlayServProperty)");
				}
				if (child is UhtProperty hookProperty && PlayServEntityModel.GetOnChangedName(hookProperty) != null)
				{
					hookProperty.LogError($"UPROPERTY(PlayServOnChanged) on '{classObj.SourceName}.{hookProperty.SourceName}' is orphaned — '{classObj.SourceName}' is not a PlayServ entity");
				}
			}

			if (PlayServEntityModel.HasPlayerOwnedMeta(classObj))
			{
				classObj.LogError($"UCLASS(PlayServPlayerOwned) on '{classObj.SourceName}' requires UCLASS(PlayServEntity) on the same class");
			}
		}

		private static string BuildGeneratedCpp(List<EntityInfo> entities, List<StructInfo> structs)
		{
			StringBuilder sb = new();
			sb.AppendLine("// Generated by PlayServUht (PlayServ SDK codegen). Do not edit.");
			sb.AppendLine("// Compile-time registration + serializer descriptor tables for every PlayServ entity");
			sb.AppendLine("// class in this target, and the declared member names of the USTRUCTs the SDK writes.");
			sb.AppendLine("// Compiled into PlayServRuntime; registration is string-based, so no game-module");
			sb.AppendLine("// linkage is required. Re-emitted on every UHT run.");
			sb.AppendLine();
			sb.AppendLine("#include \"Data/PlayServCodegenRegistry.h\"");
			sb.AppendLine();

			if (entities.Count == 0 && structs.Count == 0)
			{
				sb.AppendLine("// No PlayServ entity classes or USTRUCTs in this target.");
				return sb.ToString();
			}

			sb.AppendLine("namespace");
			sb.AppendLine("{");

			for (int s = 0; s < structs.Count; s++)
			{
				StructInfo info = structs[s];
				sb.Append($"\tconst TCHAR* const PlayServStructMembers_{s}[] = {{ ");
				for (int i = 0; i < info.MemberNames.Count; i++)
				{
					sb.Append(i > 0 ? ", " : "").Append($"TEXT(\"{info.MemberNames[i]}\")");
				}
				sb.AppendLine(" };");
				sb.AppendLine($"\tconst FPlayServCodegenStructAutoReg PlayServStructReg_{s}(TEXT(\"{info.StructPath}\"), PlayServStructMembers_{s}, {info.MemberNames.Count});");
			}
			if (structs.Count > 0)
			{
				sb.AppendLine();
			}

			foreach (EntityInfo entity in entities)
			{
				string id = entity.Class.SourceName;
				string mode = entity.Partial ? "partial" : "whole";
				int classFlags = (entity.ClientWritable ? 1 : 0) | (entity.Partial ? 2 : 0) | (entity.Singleton ? 4 : 0) | (entity.PlayerOwned ? 8 : 0);
				bool anyFlags = entity.Fields.Exists(f => f.Writable);
				bool anyHooks = entity.Fields.Exists(f => f.OnChanged != null);
				sb.AppendLine($"\t// {entity.ClassPath} (module {entity.ModuleName}, {mode}, {entity.Fields.Count} serialized fields)");

				if (entity.Fields.Count > 0)
				{
					sb.Append($"\tconst TCHAR* const PlayServFields_{id}[] = {{ ");
					for (int i = 0; i < entity.Fields.Count; i++)
					{
						sb.Append(i > 0 ? ", " : "").Append($"TEXT(\"{entity.Fields[i].Property.SourceName}\")");
					}
					sb.AppendLine(" };");

					string flagsArg = "nullptr";
					if (anyFlags)
					{
						sb.Append($"\tconst unsigned char PlayServFieldFlags_{id}[] = {{ ");
						for (int i = 0; i < entity.Fields.Count; i++)
						{
							int fieldFlags = entity.Fields[i].Writable ? 1 : 0;
							sb.Append(i > 0 ? ", " : "").Append(fieldFlags);
						}
						sb.AppendLine(" };");
						flagsArg = $"PlayServFieldFlags_{id}";
					}

					string hooksArg = "nullptr";
					if (anyHooks)
					{
						sb.Append($"\tconst TCHAR* const PlayServOnChanged_{id}[] = {{ ");
						for (int i = 0; i < entity.Fields.Count; i++)
						{
							string? hook = entity.Fields[i].OnChanged;
							sb.Append(i > 0 ? ", " : "").Append(hook != null ? $"TEXT(\"{hook}\")" : "nullptr");
						}
						sb.AppendLine(" };");
						hooksArg = $"PlayServOnChanged_{id}";
					}

					sb.AppendLine($"\tconst FPlayServCodegenAutoReg PlayServReg_{id}(TEXT(\"{entity.ClassPath}\"), PlayServFields_{id}, {flagsArg}, {hooksArg}, {entity.Fields.Count}, 0x{entity.SchemaHash:X8}u, {classFlags});");
				}
				else
				{
					sb.AppendLine($"\tconst FPlayServCodegenAutoReg PlayServReg_{id}(TEXT(\"{entity.ClassPath}\"), nullptr, nullptr, nullptr, 0, 0x{entity.SchemaHash:X8}u, {classFlags});");
				}
				sb.AppendLine();
			}

			sb.AppendLine("}");
			return sb.ToString();
		}

		private static string BuildManifest(List<EntityInfo> entities, List<EnumInfo> enums)
		{
			StringBuilder sb = new();
			sb.AppendLine("{");
			sb.AppendLine("\t\"manifestVersion\": 2,");
			sb.AppendLine("\t\"entities\": [");

			for (int i = 0; i < entities.Count; i++)
			{
				EntityInfo entity = entities[i];
				sb.AppendLine("\t\t{");
				sb.AppendLine($"\t\t\t\"classPath\": \"{entity.ClassPath}\",");
				sb.AppendLine($"\t\t\t\"class\": \"{entity.Class.SourceName}\",");
				sb.AppendLine($"\t\t\t\"module\": \"{entity.ModuleName}\",");
				sb.AppendLine($"\t\t\t\"mode\": \"{(entity.Partial ? "partial" : "whole")}\",");
				sb.AppendLine($"\t\t\t\"clientWritable\": {(entity.ClientWritable ? "true" : "false")},");
				sb.AppendLine($"\t\t\t\"singleton\": {(entity.Singleton ? "true" : "false")},");
				sb.AppendLine($"\t\t\t\"owned\": {(entity.PlayerOwned ? "true" : "false")},");
				sb.AppendLine($"\t\t\t\"schemaHash\": \"0x{entity.SchemaHash:X8}\",");
				sb.AppendLine("\t\t\t\"fields\": [");
				for (int f = 0; f < entity.Fields.Count; f++)
				{
					PlayServFieldInfo field = entity.Fields[f];
					string comma = f < entity.Fields.Count - 1 ? "," : "";
					sb.AppendLine($"\t\t\t\t{{ \"name\": \"{field.Property.SourceName}\", \"kind\": {(byte)field.Kind}, \"kindName\": \"{field.Kind}\", \"clientWritable\": {(field.Writable ? "true" : "false")}, \"decl\": \"{JsonEscape(field.Property.GetUserFacingDecl())}\" }}{comma}");
				}
				sb.AppendLine("\t\t\t]");
				sb.AppendLine(i < entities.Count - 1 ? "\t\t}," : "\t\t}");
			}

			sb.AppendLine("\t],");
			sb.AppendLine("\t\"enums\": [");
			for (int i = 0; i < enums.Count; i++)
			{
				EnumInfo enumInfo = enums[i];
				string values = string.Join(", ", enumInfo.Values.ConvertAll(v => $"\"{JsonEscape(v)}\""));
				string comma = i < enums.Count - 1 ? "," : "";
				sb.AppendLine($"\t\t{{ \"name\": \"{JsonEscape(enumInfo.Name)}\", \"codeKey\": \"{JsonEscape(enumInfo.CodeKey)}\", \"values\": [{values}] }}{comma}");
			}
			sb.AppendLine("\t]");
			sb.AppendLine("}");
			return sb.ToString();
		}

		private static string JsonEscape(string value)
		{
			return value.Replace("\\", "\\\\").Replace("\"", "\\\"");
		}

		[UhtCodeGeneratorInjector(UhtType = typeof(UhtClass), Location = UhtCodeGeneratorInjectionLocation.GeneratedMacro)]
		public static void InjectEntityMarker(StringBuilder builder, UhtType uhtType, int leadingTabs, string eolSequence)
		{
			if (uhtType is not UhtClass classObj || !PlayServEntityModel.IsEntityClass(classObj))
			{
				return;
			}

			builder.Append('\t', leadingTabs).Append("public:").Append(eolSequence);
			builder.Append('\t', leadingTabs).Append("struct FPlayServCodegenPresent {};").Append(eolSequence);
			builder.Append('\t', leadingTabs).Append("static constexpr bool bPlayServCodegen = true;").Append(eolSequence);
			builder.Append('\t', leadingTabs).Append("static constexpr bool bPlayServSingleton = ").Append(PlayServEntityModel.HasSingletonMeta(classObj) ? "true" : "false").Append(';').Append(eolSequence);
			builder.Append('\t', leadingTabs).Append("static constexpr bool bPlayServPlayerOwned = ").Append(PlayServEntityModel.HasPlayerOwnedMeta(classObj) ? "true" : "false").Append(';').Append(eolSequence);

			switch (classObj.GeneratedBodyAccessSpecifier)
			{
				case UhtAccessSpecifier.Public:
					builder.Append('\t', leadingTabs).Append("public:").Append(eolSequence);
					break;
				case UhtAccessSpecifier.Protected:
					builder.Append('\t', leadingTabs).Append("protected:").Append(eolSequence);
					break;
				default:
					builder.Append('\t', leadingTabs).Append("private:").Append(eolSequence);
					break;
			}
		}
	}
}

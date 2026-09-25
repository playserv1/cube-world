using System.Collections.Generic;
using EpicGames.Core;
using EpicGames.UHT.Parsers;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Types;
using EpicGames.UHT.Utils;

namespace PlayServUht
{
	[UnrealHeaderTool]
	public static class PlayServSpecifiers
	{
		public const string EntityMetaKey = "PlayServEntity";

		public const string ClientWritableMetaKey = "PlayServClientWritable";

		public const string PropertyMetaKey = "PlayServProperty";

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.None)]
		private static void PlayServEntitySpecifier(UhtSpecifierContext specifierContext)
		{
			specifierContext.MetaData.Add(EntityMetaKey, "true");
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.None)]
		private static void PlayServClientWritableSpecifier(UhtSpecifierContext specifierContext)
		{
			specifierContext.MetaData.Add(ClientWritableMetaKey, "true");
		}

		public const string SingletonMetaKey = "PlayServSingleton";

		public const string PlayerOwnedMetaKey = "PlayServPlayerOwned";

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.None)]
		private static void PlayServSingletonSpecifier(UhtSpecifierContext specifierContext)
		{
			specifierContext.MetaData.Add(SingletonMetaKey, "true");
			specifierContext.MetaData.Add(EntityMetaKey, "true");
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.None)]
		private static void PlayServPlayerOwnedSpecifier(UhtSpecifierContext specifierContext)
		{
			specifierContext.MetaData.Add(PlayerOwnedMetaKey, "true");
		}
	}

	[UnrealHeaderTool]
	public static class PlayServPropertySpecifiers
	{
		[UhtSpecifier(Extends = UhtTableNames.PropertyMember, ValueType = UhtSpecifierValueType.None)]
		private static void PlayServPropertySpecifier(UhtSpecifierContext specifierContext)
		{
			specifierContext.MetaData.Add(PlayServSpecifiers.PropertyMetaKey, "true");
		}

		[UhtSpecifier(Extends = UhtTableNames.PropertyMember, ValueType = UhtSpecifierValueType.None)]
		private static void PlayServClientWritableSpecifier(UhtSpecifierContext specifierContext)
		{
			specifierContext.MetaData.Add(PlayServSpecifiers.ClientWritableMetaKey, "true");
		}

		public const string OnChangedMetaKey = "PlayServOnChanged";

		[UhtSpecifier(Extends = UhtTableNames.PropertyMember, ValueType = UhtSpecifierValueType.SingleString)]
		private static void PlayServOnChangedSpecifier(UhtSpecifierContext specifierContext, StringView value)
		{
			UhtPropertySpecifierContext context = (UhtPropertySpecifierContext)specifierContext;
			if (context.PropertySettings.Outer is UhtScriptStruct)
			{
				context.MessageSite.LogError("PlayServOnChanged belongs on a top-level entity field; a change inside a USTRUCT reports through the entity field that holds the struct");
			}
			if (value.Span.Length == 0)
			{
				context.MessageSite.LogError("PlayServOnChanged needs the name of a UFUNCTION of this class: UPROPERTY(PlayServOnChanged = OnScoreChanged)");
			}
			specifierContext.MetaData.Add(OnChangedMetaKey, value.ToString());
		}
	}

	public static class PlayServEntityModel
	{
		public const string PlayerIdFieldName = "player_id";

		// Warning: the byte values are in every shipped manifest and every schema hash; renumbering one moves the hash of every entity.
		public enum EFieldKind : byte
		{
			Unsupported = 0,
			String = 1,
			Int32 = 2,
			Int64 = 3,
			Float = 4,
			Double = 5,
			Bool = 6,
			Enum = 7,
			Struct = 8,
			Array = 9,
			ObjectRef = 10,
		}

		public static bool IsEntityClass(UhtClass classObj)
		{
			return HasEntityMeta(classObj) || IsPartialEntityClass(classObj);
		}

		public static bool IsPartialEntityClass(UhtClass classObj)
		{
			if (HasEntityMeta(classObj))
			{
				return false;
			}
			foreach (UhtType child in classObj.Children)
			{
				if (child is UhtProperty property && IsMarkedProperty(property))
				{
					return true;
				}
			}
			return false;
		}

		public static bool HasEntityMeta(UhtClass classObj)
		{
			return classObj.MetaData.ContainsKey(PlayServSpecifiers.EntityMetaKey);
		}

		public static bool HasClientWritableMeta(UhtClass classObj)
		{
			return classObj.MetaData.ContainsKey(PlayServSpecifiers.ClientWritableMetaKey);
		}

		public static bool IsMarkedProperty(UhtProperty property)
		{
			return property.MetaData.ContainsKey(PlayServSpecifiers.PropertyMetaKey);
		}

		public static bool IsPropertyClientWritable(UhtProperty property)
		{
			return property.MetaData.ContainsKey(PlayServSpecifiers.ClientWritableMetaKey);
		}

		public static bool HasSingletonMeta(UhtClass classObj)
		{
			return classObj.MetaData.ContainsKey(PlayServSpecifiers.SingletonMetaKey);
		}

		public static bool HasPlayerOwnedMeta(UhtClass classObj)
		{
			return classObj.MetaData.ContainsKey(PlayServSpecifiers.PlayerOwnedMetaKey);
		}

		public static string? GetOnChangedName(UhtProperty property)
		{
			return property.MetaData.TryGetValue(PlayServPropertySpecifiers.OnChangedMetaKey, out string? name) ? name : null;
		}

		public static bool IsSerializedField(UhtProperty property)
		{
			return !property.PropertyFlags.HasAnyFlags(EPropertyFlags.Transient | EPropertyFlags.EditorOnly);
		}

		public static EFieldKind Classify(UhtProperty property)
		{
			switch (property)
			{
				case UhtStrProperty: return EFieldKind.String;
				case UhtIntProperty: return EFieldKind.Int32;
				case UhtInt64Property: return EFieldKind.Int64;
				case UhtFloatProperty: return EFieldKind.Float;
				case UhtDoubleProperty: return EFieldKind.Double;
				case UhtBoolProperty: return EFieldKind.Bool;
				case UhtEnumProperty: return EFieldKind.Enum;
				case UhtStructProperty: return EFieldKind.Struct;
				case UhtArrayProperty: return EFieldKind.Array;
				// Warning: the soft and lazy pointer cases must come before the object cases they derive from.
				case UhtSoftClassProperty: return EFieldKind.Unsupported;
				case UhtSoftObjectProperty: return EFieldKind.Unsupported;
				case UhtLazyObjectPtrProperty: return EFieldKind.Unsupported;
				case UhtClassProperty: return EFieldKind.ObjectRef;
				case UhtObjectProperty: return EFieldKind.ObjectRef;
				default: return EFieldKind.Unsupported;
			}
		}

		public static List<PlayServFieldInfo> CollectFields(UhtClass classObj, bool bWholeMode)
		{
			List<PlayServFieldInfo> fields = new();
			for (UhtClass? current = classObj; current != null; current = current.SuperClass)
			{
				foreach (UhtType child in current.Children)
				{
					if (child is UhtProperty property && IsSerializedField(property)
						&& (bWholeMode || IsMarkedProperty(property)))
					{
						fields.Add(new PlayServFieldInfo(property, current, Classify(property), IsPropertyClientWritable(property), GetOnChangedName(property)));
					}
				}
			}
			return fields;
		}

		public static string GetClassPath(UhtPackage package, UhtClass classObj)
		{
			return $"{package.SourceName}.{classObj.EngineName}";
		}

		public static uint ComputeSchemaHash(List<PlayServFieldInfo> fields)
		{
			uint hash = 2166136261u;
			foreach (PlayServFieldInfo field in fields)
			{
				foreach (char c in field.Property.SourceName)
				{
					hash = (hash ^ c) * 16777619u;
				}
				hash = (hash ^ ':') * 16777619u;
				hash = (hash ^ (uint)((byte)field.Kind | (field.Writable ? 0x80 : 0))) * 16777619u;
				hash = (hash ^ ';') * 16777619u;
			}
			return hash;
		}
	}

	public sealed record PlayServFieldInfo(UhtProperty Property, UhtClass DeclaringClass, PlayServEntityModel.EFieldKind Kind, bool Writable, string? OnChanged);
}

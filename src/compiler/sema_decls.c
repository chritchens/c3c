// Copyright (c) 2020 Christoffer Lerno. All rights reserved.
// Use of this source code is governed by a LGPLv3.0
// a copy of which can be found in the LICENSE file.

#include "sema_internal.h"
#include "bigint.h"





static AttributeType sema_analyse_attribute(Context *context, Attr *attr, AttributeDomain domain);


static bool sema_analyse_struct_union(Context *context, Decl *decl);

static inline bool sema_analyse_struct_member(Context *context, Decl *decl)
{
	if (decl->name)
	{
		Decl *other = sema_resolve_symbol_in_current_dynamic_scope(context, decl->name);
		if (other)
		{
			SEMA_ERROR(decl, "Duplicate member name '%s'.", other->name);
			SEMA_PREV(other, "Previous declaration was here.");
			return false;
		}
		if (decl->name) sema_add_member(context, decl);
	}
	switch (decl->decl_kind)
	{
		case DECL_VAR:
			assert(decl->var.kind == VARDECL_MEMBER);
			decl->resolve_status = RESOLVE_RUNNING;
			if (!sema_resolve_type_info(context, decl->var.type_info)) return decl_poison(decl);
			decl->type = decl->var.type_info->type;
			decl->resolve_status = RESOLVE_DONE;
			return true;
		case DECL_STRUCT:
		case DECL_UNION:
			return sema_analyse_decl(context, decl);
		default:
			UNREACHABLE
	}
}

static bool sema_analyse_union_members(Context *context, Decl *decl, Decl **members)
{
	ByteSize max_size = 0;
	MemberIndex max_alignment_element = 0;
	AlignSize max_alignment = 0;

	VECEACH(members, i)
	{
		Decl *member = members[i];
		if (!decl_ok(member))
		{
			decl_poison(decl);
			continue;
		}
		if (!sema_analyse_struct_member(context, member))
		{
			if (decl_ok(decl))
			{
				decl_poison(decl);
				continue;
			}
			continue;
		}

		AlignSize member_alignment = type_abi_alignment(member->type);
		ByteSize member_size = type_size(member->type);

		// Update max alignment
		if (member_alignment > max_alignment)
		{
			max_alignment = member_alignment;
			max_alignment_element = i;
		}
		// Update max size
		if (member_size > max_size)
		{
			//max_size_element = i;
			max_size = member_size;
			// If this is bigger than the previous with max
			// alignment, pick this as the maximum size field.
			if (max_alignment_element != (MemberIndex)i && max_alignment == member_alignment)
			{
				max_alignment_element = (MemberIndex)i;
			}
		}
		// Offset is always 0
		member->offset = 0;
	}

	if (!decl_ok(decl)) return false;

	// 1. If packed, then the alignment is zero, unless previously given
	if (decl->is_packed && !decl->alignment) decl->alignment = 1;

	// 2. Otherwise pick the highest of the natural alignment and the given alignment.
	if (!decl->is_packed) decl->alignment = MAX(decl->alignment, max_alignment);

	// We're only packed if the max alignment is > 1
	decl->is_packed = decl->is_packed && max_alignment > 1;

	decl->strukt.union_rep = max_alignment_element;

	if (!max_size)
	{
		decl->strukt.size = 0;
		decl->alignment = 1;
		return true;
	}

	// The actual size might be larger than the max size due to alignment.
	unsigned size = aligned_offset(max_size, decl->alignment);

	unsigned rep_size = type_size(members[max_alignment_element]->type);

	// If the actual size is bigger than the real size, add
	// padding.
	if (size > rep_size)
	{
		decl->strukt.padding = size - rep_size;
	}

	decl->strukt.size = size;

	return true;
}

static bool sema_analyse_struct_members(Context *context, Decl *decl, Decl **members)
{
	// Default alignment is 1 even if the it is empty.
	AlignSize natural_alignment = 1;
	bool is_unaligned = false;
	ByteSize size = 0;
	ByteSize offset = 0;
	bool is_packed = decl->is_packed;
	VECEACH(members, i)
	{
		Decl *member = decl->strukt.members[i];
		if (!decl_ok(member))
		{
			decl_poison(decl);
			continue;
		}
		if (!sema_analyse_struct_member(context, member))
		{
			if (decl_ok(decl))
			{
				decl_poison(decl);
				continue;
			}
			continue;
		}

		if (!decl_ok(decl)) return false;

		AlignSize member_natural_alignment = type_abi_alignment(member->type);
		AlignSize member_alignment = is_packed ? 1 : member_natural_alignment;
		Attr **attributes = member->attributes;
		unsigned count = vec_size(attributes);
		for (unsigned j = 0; j < count; j++)
		{
			Attr *attribute = attributes[j];
			if (!sema_analyse_attribute(context, attribute, ATTR_VAR)) return false;
			if (TOKSTR(attribute->name) == kw_align)
			{
				member_alignment = attribute->alignment;
				// Update total alignment if we have a member that has bigger alignment.
				if (member_alignment > decl->alignment) decl->alignment = member_alignment;
			}
		}

		// If the member alignment is higher than the currently detected alignment,
		// then we update the natural alignment
		if (member_natural_alignment > natural_alignment)
		{
			natural_alignment = member_natural_alignment;
		}

		// In the case of a struct, we will align this to the next offset,
		// using the alignment of the member.
		unsigned align_offset = aligned_offset(offset, member_alignment);

		unsigned natural_align_offset = aligned_offset(offset, member_natural_alignment);

		// If the natural align is different from the aligned offset we have two cases:
		if (natural_align_offset != align_offset)
		{
			// If the natural alignment is greater, in this case the struct is unaligned.
			if (member_natural_alignment > member_alignment)
			{
				assert(natural_align_offset > align_offset);
				is_unaligned = true;
			}
			else
			{
				// Otherwise we have a greater offset, and in this case
				// we add padding for the difference.
				assert(natural_align_offset < align_offset);
				member->padding = align_offset - offset;
			}
		}

		offset = align_offset;
		member->offset = offset;
		offset += type_size(member->type);
	}

	// Set the alignment:

	// 1. If packed, use the alignment given, otherwise set to 1.
	if (decl->is_packed && !decl->alignment) decl->alignment = 1;

	// 2. Otherwise pick the highest of the natural alignment and the given alignment.
	if (!decl->is_packed) decl->alignment = MAX(decl->alignment, natural_alignment);

	// We must now possibly add the end padding.
	// First we calculate the actual size
	size = aligned_offset(offset, decl->alignment);

	// We might get a size that is greater than the natural alignment
	// in this case we need an additional padding
	if (size > aligned_offset(offset, natural_alignment))
	{
		decl->strukt.padding = size - offset;
	}

	// If the size is smaller the naturally aligned struct, then it is also unaligned
	if (size < aligned_offset(offset, natural_alignment))
	{
		is_unaligned = true;
	}
	if (is_unaligned && size > offset)
	{
		assert(!decl->strukt.padding);
		decl->strukt.padding = size - offset;
	}
	decl->is_packed = is_unaligned;
	decl->strukt.size = size;
	return true;
}

static bool sema_analyse_struct_union(Context *context, Decl *decl)
{
	AttributeDomain domain;
	switch (decl->decl_kind)
	{
		case DECL_STRUCT:
			domain = ATTR_STRUCT;
			break;
		case DECL_UNION:
			domain = ATTR_UNION;
			break;
		case DECL_ERR:
			domain = ATTR_ERROR;
			break;
		default:
			UNREACHABLE
	}
	VECEACH(decl->attributes, i)
	{
		Attr *attr = decl->attributes[i];

		AttributeType attribute = sema_analyse_attribute(context, attr, domain);
		if (attribute == ATTRIBUTE_NONE) return decl_poison(decl);

		bool had = false;
#define SET_ATTR(_X) had = decl->func._X; decl->func._X = true; break
		switch (attribute)
		{
			case ATTRIBUTE_CNAME:
				had = decl->cname != NULL;
				decl->cname = attr->expr->const_expr.string.chars;
				break;
			case ATTRIBUTE_SECTION:
				had = decl->section != NULL;
				decl->section = attr->expr->const_expr.string.chars;
				break;
			case ATTRIBUTE_ALIGN:
				had = decl->alignment != 0;
				decl->alignment = attr->alignment;
				break;
			case ATTRIBUTE_PACKED:
				had = decl->is_packed;
				decl->is_packed = true;
				break;
			case ATTRIBUTE_OPAQUE:
				had = decl->is_opaque;
				decl->is_opaque = true;
				break;
			default:
				UNREACHABLE
		}
#undef SET_ATTR
		if (had)
		{
			SEMA_TOKID_ERROR(attr->name, "Attribute occurred twice, please remove one.");
			return decl_poison(decl);
		}
	}

	DEBUG_LOG("Beginning analysis of %s.", decl->name ? decl->name : "anon");
	if (decl->name) context_push_scope(context);
	bool success;
	if (decl->decl_kind == DECL_UNION)
	{
		success = sema_analyse_union_members(context, decl, decl->strukt.members);
	}
	else
	{
		success = sema_analyse_struct_members(context, decl, decl->strukt.members);
	}
	DEBUG_LOG("Struct/union size %d, alignment %d.", (int)decl->strukt.size, (int)decl->alignment);
	if (decl->name) context_pop_scope(context);
	DEBUG_LOG("Analysis complete.");
	if (!success) return decl_poison(decl);
	return decl_ok(decl);
}


static inline bool sema_analyse_function_param(Context *context, Decl *param, bool is_function, bool *has_default)
{
	*has_default = false;
	assert(param->decl_kind == DECL_VAR);
	assert(param->var.kind == VARDECL_PARAM);
	if (!sema_resolve_type_info(context, param->var.type_info))
	{
		return false;
	}
	if (param->var.vararg)
	{
		param->var.type_info->type = type_get_subarray(param->var.type_info->type);
	}
	param->type = param->var.type_info->type;
	if (param->var.init_expr && !is_function)
	{
		SEMA_ERROR(param->var.init_expr, "Function types may not have default arguments.");
		return false;
	}
	if (param->var.init_expr)
	{
		Expr *expr = param->var.init_expr;
		if (!sema_analyse_expr_of_required_type(context, param->type, expr, false)) return false;
		if (expr->expr_kind != EXPR_CONST)
		{
			SEMA_ERROR(expr, "Only constant expressions may be used as default values.");
			return false;
		}
		*has_default = true;
	}
	param->alignment = type_abi_alignment(param->type);
	return true;
}

static inline Type *sema_analyse_function_signature(Context *context, FunctionSignature *signature, bool is_function)
{
	char buffer[MAX_FUNCTION_SIGNATURE_SIZE + 200];
	size_t buffer_write_offset = 0;
	bool all_ok = true;
	all_ok = sema_resolve_type_info(context, signature->rtype) && all_ok;
	if (all_ok)
	{
		type_append_signature_name(signature->rtype->type, buffer, &buffer_write_offset);
		if (signature->failable) buffer[buffer_write_offset++] = '!';
		buffer[buffer_write_offset++] = '(';
	}
	if (vec_size(signature->params) > MAX_PARAMS)
	{
		SEMA_ERROR(signature->params[MAX_PARAMS], "Number of params exceeds %d which is unsupported.", MAX_PARAMS);
		return false;
	}
	STable *names = &global_context.scratch_table;
	stable_clear(names);

	VECEACH(signature->params, i)
	{
		Decl *param = signature->params[i];
		assert(param->resolve_status == RESOLVE_NOT_DONE);
		param->resolve_status = RESOLVE_RUNNING;
		bool has_default;
		if (!sema_analyse_function_param(context, param, is_function, &has_default))
		{
			decl_poison(param);
			all_ok = false;
			continue;
		}
		signature->has_default = signature->has_default || has_default;
		param->resolve_status = RESOLVE_DONE;
		if (i > 0 && all_ok)
		{
			buffer[buffer_write_offset++] = ',';
		}
		type_append_signature_name(param->var.type_info->type, buffer, &buffer_write_offset);
		if (param->name)
		{
			Decl *prev = stable_set(names, param->name, param);
			if (prev)
			{
				SEMA_ERROR(param, "Duplicate parameter name %s.", param->name);
				SEMA_PREV(prev, "Previous use of the name was here.");
				decl_poison(prev);
				decl_poison(param);
				all_ok = false;
			}
		}
	}
	if (signature->variadic)
	{
		buffer[buffer_write_offset++] = ',';
		buffer[buffer_write_offset++] = '.';
		buffer[buffer_write_offset++] = '.';
		buffer[buffer_write_offset++] = '.';
	}
	buffer[buffer_write_offset++] = ')';

	if (!all_ok) return NULL;

	TokenType type = TOKEN_INVALID_TOKEN;
	const char *mangled_signature = symtab_add(buffer, buffer_write_offset, fnv1a(buffer, buffer_write_offset), &type);
	Type *func_type = stable_get(&context->local_symbols, mangled_signature);
	c_abi_func_create(signature);
	if (!func_type)
	{
		func_type = type_new(TYPE_FUNC, mangled_signature);
		func_type->canonical = func_type;
		func_type->func.signature = signature;
		func_type->func.mangled_function_signature = mangled_signature;
		stable_set(&context->local_symbols, mangled_signature, func_type);
	}
	return func_type;

}

static inline bool sema_analyse_typedef(Context *context, Decl *decl)
{
	if (decl->typedef_decl.is_func)
	{
		Type *func_type = sema_analyse_function_signature(context, &decl->typedef_decl.function_signature, false);
		if (!func_type) return false;
		decl->type->canonical = type_get_ptr(func_type);
		return true;
	}
	if (!sema_resolve_type_info(context, decl->typedef_decl.type_info)) return false;
	decl->type->canonical = decl->typedef_decl.type_info->type->canonical;
	// Do we need anything else?
	return true;
}

static inline bool sema_analyse_distinct(Context *context, Decl *decl)
{
	if (decl->distinct_decl.typedef_decl.is_func)
	{
		Type *func_type = sema_analyse_function_signature(context, &decl->distinct_decl.typedef_decl.function_signature, false);
		if (!func_type) return false;
		decl->distinct_decl.base_type = type_get_ptr(func_type);
		return true;
	}
	TypeInfo *info = decl->distinct_decl.typedef_decl.type_info;
	if (!sema_resolve_type_info(context, info)) return false;
	Type *base = type_flatten_distinct(info->type);
	decl->distinct_decl.base_type = base;
	switch (base->type_kind)
	{
		case TYPE_POISONED:
		case TYPE_STRLIT:
		case TYPE_IXX:
		case TYPE_FXX:
		case TYPE_FUNC:
		case TYPE_TYPEDEF:
		case TYPE_DISTINCT:
		case TYPE_INFERRED_ARRAY:
		case TYPE_TYPEINFO:
		case TYPE_MEMBER:
			UNREACHABLE
			return false;
		case TYPE_VIRTUAL_ANY:
		case TYPE_VIRTUAL:
			SEMA_ERROR(decl, "You cannot create a distinct type from a virtual type.");
			return false;
		case TYPE_ERRTYPE:
			SEMA_ERROR(decl, "You cannot create a distinct type from an error.");
			return false;
		case TYPE_ERR_UNION:
			SEMA_ERROR(decl, "You cannot create a distinct type from an error union.");
			return false;
		case TYPE_VOID:
		case TYPE_TYPEID:
			SEMA_ERROR(decl, "Cannot create a distinct type from %s.", type_quoted_error_string(base));
		case TYPE_BOOL:
		case ALL_SIGNED_INTS:
		case ALL_UNSIGNED_INTS:
		case ALL_REAL_FLOATS:
		case TYPE_POINTER:
		case TYPE_ENUM:
		case TYPE_STRUCT:
		case TYPE_UNION:
		case TYPE_ARRAY:
		case TYPE_VARARRAY:
		case TYPE_SUBARRAY:
		case TYPE_VECTOR:
		case TYPE_COMPLEX:
			break;
	}
	// Do we need anything else?
	return true;
}

static inline bool sema_analyse_enum(Context *context, Decl *decl)
{
	// Resolve the type of the enum.
	if (!sema_resolve_type_info(context, decl->enums.type_info)) return false;

	Type *type = decl->enums.type_info->type;
	Type *canonical = type->canonical;

	// Require an integer type
	if (!type_is_integer(canonical))
	{
		SEMA_ERROR(decl->enums.type_info, "The enum type must be an integer type not '%s'.", type_to_error_string(type));
		return false;
	}

	DEBUG_LOG("* Enum type resolved to %s.", type->name);
	bool success = true;
	unsigned enums = vec_size(decl->enums.values);
	BigInt value;
	BigInt add;
	bigint_init_unsigned(&add, 1);
	bigint_init_unsigned(&value, 0);

	for (unsigned i = 0; i < enums; i++)
	{
		Decl *enum_value = decl->enums.values[i];
		enum_value->type = decl->type;
		DEBUG_LOG("* Checking enum constant %s.", enum_value->name);
		enum_value->enum_constant.ordinal = i;
		DEBUG_LOG("* Ordinal: %d", i);
		assert(enum_value->resolve_status == RESOLVE_NOT_DONE);
		assert(enum_value->decl_kind == DECL_ENUM_CONSTANT);

		// Start evaluating the constant
		enum_value->resolve_status = RESOLVE_RUNNING;
		Expr *expr = enum_value->enum_constant.expr;

		// Create a "fake" expression.
		// This will be evaluated later to catch the case
		if (!expr)
		{
			expr = expr_new(EXPR_CONST, source_span_from_token_id(enum_value->name_token));
			expr_set_type(expr, type);
			expr->resolve_status = RESOLVE_NOT_DONE;
			bigint_init_bigint(&expr->const_expr.i, &value);
			expr->const_expr.kind = TYPE_IXX;
			expr_set_type(expr, type_compint);
			enum_value->enum_constant.expr = expr;
		}

		// We try to convert to the desired type.
		if (!sema_analyse_expr_of_required_type(context, type, expr, false))
		{
			success = false;
			enum_value->resolve_status = RESOLVE_DONE;
			decl_poison(enum_value);
			// Reset!
			bigint_init_unsigned(&value, 0);
			continue;
		}

		assert(type_is_integer(expr->type->canonical));

		// Here we might have a non-constant value,
		if (expr->expr_kind != EXPR_CONST)
		{
			SEMA_ERROR(expr, "Expected a constant expression for enum.");
			decl_poison(enum_value);
			success = false;
			// Skip one value.
			continue;
		}

		// Update the value
		bigint_add(&value, &expr->const_expr.i, &add);
		DEBUG_LOG("* Value: %s", expr_const_to_error_string(&expr->const_expr));
		enum_value->resolve_status = RESOLVE_DONE;
	}
	return success;
}



static inline bool sema_analyse_method(Context *context, Decl *decl)
{
	TypeInfo *parent_type = decl->func.type_parent;
	if (!sema_resolve_type_info(context, parent_type)) return false;
	if (!type_may_have_sub_elements(parent_type->type))
	{
		SEMA_ERROR(decl,
		               "Methods can not be associated with '%s'",
		               type_to_error_string(decl->func.type_parent->type));
		return false;
	}
	Decl *parent = parent_type->type->decl;
	VECEACH(parent->methods, i)
	{
		Decl *function = parent->methods[i];
		if (function->name == decl->name)
		{
			SEMA_ERROR(decl, "Duplicate name '%s' for method.", function->name);
			SEMA_PREV(function, "Previous definition here.");
			return false;
		}
	}
	DEBUG_LOG("Method '%s.%s' analysed.", parent->name, decl->name);
	vec_add(parent->methods, decl);
	return true;
}

static inline AttributeType attribute_by_name(Attr *attr)
{
	const char *attribute = TOKSTR(attr->name);
	for (unsigned i = 0; i < NUMBER_OF_ATTRIBUTES; i++)
	{
		if (attribute_list[i] == attribute) return (AttributeType)i;
	}
	return ATTRIBUTE_NONE;
}

static const char *attribute_domain_to_string(AttributeDomain domain)
{
	switch (domain)
	{
		case ATTR_INTERFACE:
			return "interface";
		case ATTR_MEMBER:
			return "member";
		case ATTR_FUNC:
			return "function";
		case ATTR_VAR:
			return "variable";
		case ATTR_ENUM:
			return "enum";
		case ATTR_STRUCT:
			return "struct";
		case ATTR_UNION:
			return "union";
		case ATTR_CONST:
			return "constant";
		case ATTR_ERROR:
			return "error type";
		case ATTR_TYPEDEF:
			return "typedef";
	}
	UNREACHABLE
}
static AttributeType sema_analyse_attribute(Context *context, Attr *attr, AttributeDomain domain)
{
	AttributeType type = attribute_by_name(attr);
	if (type == ATTRIBUTE_NONE)
	{
		SEMA_TOKID_ERROR(attr->name, "There is no attribute with the name '%s', did you mistype?", TOKSTR(attr->name));
		return ATTRIBUTE_NONE;
	}
	static AttributeDomain attribute_domain[NUMBER_OF_ATTRIBUTES] = {
			[ATTRIBUTE_WEAK] = ATTR_FUNC | ATTR_CONST | ATTR_VAR,
			[ATTRIBUTE_CNAME] = ~0,
			[ATTRIBUTE_SECTION] = ATTR_FUNC | ATTR_CONST | ATTR_VAR,
			[ATTRIBUTE_PACKED] = ATTR_STRUCT | ATTR_UNION | ATTR_ERROR,
			[ATTRIBUTE_NORETURN] = ATTR_FUNC,
			[ATTRIBUTE_ALIGN] = ATTR_FUNC | ATTR_CONST | ATTR_VAR | ATTR_STRUCT | ATTR_UNION | ATTR_MEMBER,
			[ATTRIBUTE_INLINE] = ATTR_FUNC,
			[ATTRIBUTE_NOINLINE] = ATTR_FUNC,
			[ATTRIBUTE_OPAQUE] = ATTR_STRUCT | ATTR_UNION,
			[ATTRIBUTE_STDCALL] = ATTR_FUNC
	};

	if ((attribute_domain[type] & domain) != domain)
	{
		SEMA_TOKID_ERROR(attr->name, "'%s' is not a valid %s attribute.", TOKSTR(attr->name), attribute_domain_to_string(domain));
		return ATTRIBUTE_NONE;
	}
	switch (type)
	{
		case ATTRIBUTE_STDCALL:
			return type;
		case ATTRIBUTE_ALIGN:
			if (!attr->expr)
			{
				SEMA_TOKID_ERROR(attr->name, "'align' requires an power-of-2 argument, e.g. align(8).");
				return ATTRIBUTE_NONE;
			}
			if (!sema_analyse_expr(context, type_usize, attr->expr)) return false;
			if (attr->expr->expr_kind != EXPR_CONST || !type_is_any_integer(attr->expr->type->canonical))
			{
				SEMA_ERROR(attr->expr, "Expected a constant integer value as argument.");
				return ATTRIBUTE_NONE;
			}
			{
				BigInt comp;
				bigint_init_unsigned(&comp, MAX_ALIGNMENT);
				if (bigint_cmp(&attr->expr->const_expr.i, &comp) == CMP_GT)
				{
					SEMA_ERROR(attr->expr, "Alignment must be less or equal to %ull.", MAX_ALIGNMENT);
					return ATTRIBUTE_NONE;
				}
				if (bigint_cmp_zero(&attr->expr->const_expr.i) != CMP_GT)
				{
					SEMA_ERROR(attr->expr, "Alignment must be greater than zero.");
					return ATTRIBUTE_NONE;
				}
				uint64_t align = bigint_as_unsigned(&attr->expr->const_expr.i);
				if (!is_power_of_two(align))
				{
					SEMA_ERROR(attr->expr, "Alignment must be a power of two.");
					return ATTRIBUTE_NONE;
				}

				attr->alignment = align;
			}
			return type;
		case ATTRIBUTE_SECTION:
		case ATTRIBUTE_CNAME:
			if (context->module->is_generic)
			{
				SEMA_TOKID_ERROR(attr->name, "'cname' attributes are not allowed in generic modules.");
				return false;
			}
			if (!attr->expr)
			{
				SEMA_TOKID_ERROR(attr->name, "'%s' requires a string argument, e.g. %s(\"foo\").", TOKSTR(attr->name), TOKSTR(attr->name));
				return ATTRIBUTE_NONE;
			}
			if (!sema_analyse_expr(context, NULL, attr->expr)) return false;
			if (attr->expr->expr_kind != EXPR_CONST || attr->expr->type->canonical != type_compstr)
			{
				SEMA_ERROR(attr->expr, "Expected a constant string value as argument.");
				return ATTRIBUTE_NONE;
			}
			return type;
		default:
			if (attr->expr)
			{
				SEMA_ERROR(attr->expr, "'%s' should not have any arguments.", TOKSTR(attr->name));
				return ATTRIBUTE_NONE;
			}
			return type;
	}

}

static inline bool sema_analyse_func(Context *context, Decl *decl)
{
	DEBUG_LOG("----Analysing function %s", decl->name);
	Type *func_type = sema_analyse_function_signature(context, &decl->func.function_signature, true);
	decl->type = func_type;
	if (!func_type) return decl_poison(decl);
	if (decl->func.type_parent)
	{
		if (!sema_analyse_method(context, decl)) return decl_poison(decl);
	}
	VECEACH(decl->attributes, i)
	{
		Attr *attr = decl->attributes[i];

		AttributeType attribute = sema_analyse_attribute(context, attr, ATTR_FUNC);
		if (attribute == ATTRIBUTE_NONE) return decl_poison(decl);

		bool had = false;
#define SET_ATTR(_X) had = decl->func._X; decl->func._X = true; break
		switch (attribute)
		{
			case ATTRIBUTE_CNAME:
				had = decl->cname != NULL;
				decl->cname = attr->expr->const_expr.string.chars;
				break;
			case ATTRIBUTE_SECTION:
				had = decl->section != NULL;
				decl->section = attr->expr->const_expr.string.chars;
				break;
			case ATTRIBUTE_ALIGN:
				had = decl->alignment != 0;
				decl->alignment = attr->alignment;
				break;
			case ATTRIBUTE_NOINLINE: SET_ATTR(attr_noinline);
			case ATTRIBUTE_STDCALL: SET_ATTR(attr_stdcall);
			case ATTRIBUTE_INLINE: SET_ATTR(attr_inline);
			case ATTRIBUTE_NORETURN: SET_ATTR(attr_noreturn);
			case ATTRIBUTE_WEAK: SET_ATTR(attr_weak);
			default:
				UNREACHABLE
		}
#undef SET_ATTR
		if (had)
		{
			SEMA_TOKID_ERROR(attr->name, "Attribute occurred twice, please remove one.");
			return decl_poison(decl);
		}
		if (decl->func.attr_inline && decl->func.attr_noinline)
		{
			SEMA_TOKID_ERROR(attr->name, "A function cannot be 'inline' and 'noinline' at the same time.");
			return decl_poison(decl);
		}
	}
	if (decl->name == kw_main)
	{
		if (decl->visibility == VISIBLE_LOCAL)
		{
			SEMA_ERROR(decl, "'main' cannot have local visibility.");
			return false;
		}
		decl->visibility = VISIBLE_EXTERN;
	}
	DEBUG_LOG("Function analysis done.");
	return true;
}

static inline bool sema_analyse_macro(Context *context, Decl *decl)
{
	TypeInfo *rtype = decl->macro_decl.rtype;
	if (decl->macro_decl.rtype && !sema_resolve_type_info(context, rtype)) return false;
	VECEACH(decl->macro_decl.parameters, i)
	{
		Decl *param = decl->macro_decl.parameters[i];
		assert(param->decl_kind == DECL_VAR);
		switch (param->var.kind)
		{
			case VARDECL_PARAM:
			case VARDECL_PARAM_EXPR:
			case VARDECL_PARAM_CT:
			case VARDECL_PARAM_REF:
				if (param->var.type_info && !sema_resolve_type_info(context, param->var.type_info)) return false;
				break;
			case VARDECL_PARAM_CT_TYPE:
				if (param->var.type_info)
				{
					SEMA_ERROR(param->var.type_info, "A compile time type parameter cannot have a type itself.");
					return false;
				}
				break;
			case VARDECL_CONST:
			case VARDECL_GLOBAL:
			case VARDECL_LOCAL:
			case VARDECL_MEMBER:
			case VARDECL_LOCAL_CT:
			case VARDECL_LOCAL_CT_TYPE:
			case VARDECL_ALIAS:
				UNREACHABLE
		}
	}
	return true;
}




static inline bool sema_analyse_global(Context *context, Decl *decl)
{
	if (decl->var.type_info)
	{
		if (!sema_resolve_type_info_maybe_inferred(context, decl->var.type_info, decl->var.init_expr != NULL)) return false;
		decl->type = decl->var.type_info->type;
	}

	// We expect a constant to actually be parsed correctly so that it has a value, so
	// this should always be true.
	assert(decl->type || decl->var.kind == VARDECL_CONST);

	AttributeDomain domain = decl->var.kind == VARDECL_CONST ? ATTR_CONST : ATTR_FUNC;
	VECEACH(decl->attributes, i)
	{
		Attr *attr = decl->attributes[i];

		AttributeType attribute = sema_analyse_attribute(context, attr, domain);
		if (attribute == ATTRIBUTE_NONE) return decl_poison(decl);

		bool had = false;
#define SET_ATTR(_X) had = decl->func._X; decl->func._X = true; break
		switch (attribute)
		{
			case ATTRIBUTE_CNAME:
				had = decl->cname != NULL;
				decl->cname = attr->expr->const_expr.string.chars;
				break;
			case ATTRIBUTE_SECTION:
				had = decl->section != NULL;
				decl->section = attr->expr->const_expr.string.chars;
				break;
			case ATTRIBUTE_ALIGN:
				had = decl->alignment != 0;
				decl->alignment = attr->alignment;
				break;
			case ATTRIBUTE_WEAK: SET_ATTR(attr_weak);
			default:
				UNREACHABLE
		}
#undef SET_ATTR
		if (had)
		{
			SEMA_TOKID_ERROR(attr->name, "Attribute occurred twice, please remove one.");
			return decl_poison(decl);
		}
	}

	// If we already have the type resolved then we can pretend to be done,
	// this will help in case we otherwise would get circular references.
	if (decl->type)
	{
		decl->resolve_status = RESOLVE_DONE;
		if (!decl->alignment) decl->alignment = type_alloca_alignment(decl->type);
	}

	// Check the initializer.
	if (decl->var.init_expr && decl->type)
	{
		Expr *init_expr = decl->var.init_expr;

		// 1. Check type.
		if (!sema_analyse_expr_of_required_type(context, decl->type, init_expr, false)) return false;

		// 2. Check const-ness
		if (!init_expr->constant)
		{
			// 3. Special case is when the init expression is the reference
			// to a constant global structure.
			if (init_expr->expr_kind == EXPR_CONST_IDENTIFIER)
			{
				// 4. If so we copy the init expression, which should always be constant.
				*init_expr = *init_expr->identifier_expr.decl->var.init_expr;
				assert(init_expr->constant);
			}
			else
			{
				if (init_expr->expr_kind == EXPR_CAST)
				{
					SEMA_ERROR(init_expr, "The expression may not be a non constant cast.");
				}
				else
				{
					SEMA_ERROR(init_expr, "The expression must be a constant value.");
				}
				return false;
			}
		}

		if (decl->type->type_kind == TYPE_INFERRED_ARRAY)
		{
			assert(init_expr->type->canonical->type_kind == TYPE_ARRAY);
			decl->type = type_get_array(decl->type->array.base, init_expr->type->canonical->array.len);
		}
	}

	switch (decl->var.kind)
	{
		case VARDECL_CONST:
			assert(decl->var.init_expr);
			return true;
		case VARDECL_GLOBAL:
			return true;
		default:
			eprintf("Decl %s %d\n", decl->name, decl->var.kind);
			UNREACHABLE
	}
}

static inline bool sema_analyse_generic(Context *context, Decl *decl)
{
	// 1. If it has a return type, make sure it resolves.
	if (decl->generic_decl.rtype && !sema_resolve_type_info(context, decl->generic_decl.rtype)) return false;

	unsigned param_count = vec_size(decl->generic_decl.parameters);
	if (param_count < 1)
	{
		SEMA_ERROR(decl, "A generic function needs at least 1 parameter.");
		return false;
	}
	Ast **cases = decl->generic_decl.cases;

	bool default_has_been_found = false;
	VECEACH(cases, i)
	{
		Ast *generic_case = cases[i];
		if (generic_case->ast_kind == AST_CASE_STMT)
		{
			if (!generic_case->case_stmt.is_type)
			{
				SEMA_ERROR(generic_case->case_stmt.expr, "Expected a type as the argument.");
				return false;
			}
			if (!generic_case->case_stmt.is_type_list)
			{
				TypeInfo **type_infos = VECNEW(TypeInfo *, 2);
				vec_add(type_infos, generic_case->case_stmt.type_info);
				generic_case->case_stmt.type_infos = type_infos;
				generic_case->case_stmt.is_type_list = true;
			}
			TypeInfo **type_infos = generic_case->case_stmt.type_infos;
			unsigned args = vec_size(type_infos);
			for (unsigned j = 0; j < args; j++)
			{
				if (!sema_resolve_type_info(context, type_infos[j])) return false;
			}
			if (args != param_count)
			{
				if (param_count == 1)
				{
					SEMA_ERROR(type_infos[1], "Expected a single type as the argument.");
					return false;
				}
				SEMA_ERROR(type_infos[args - 1], "Expected %d types in the case statement.", param_count);
				return false;
			}
			continue;
		}
		assert(generic_case->ast_kind == AST_DEFAULT_STMT);
		if (default_has_been_found)
		{
			SEMA_ERROR(generic_case, "More than one default statement found.");
			return false;
		}
	}
	return true;
}


static bool sema_analyse_plain_define(Context *c, Decl *decl, Decl *symbol)
{
	unsigned parameter_count = vec_size(symbol->module->parameters);
	if (parameter_count > 0)
	{
		SEMA_ERROR(decl, "Using 'define' with parameterized modules, requires parameters - did you forget them?");
		return false;
	}
	TODO
}

static Context *copy_context(Module *module, Context *c)
{
	Context *copy = context_create(c->file);
	copy->imports = copy_decl_list(c->imports);
	copy->global_decls = copy_decl_list(c->global_decls);
	copy->module = module;
	assert(!c->functions && !c->methods && !c->enums && !c->ct_ifs && !c->types && !c->interfaces && !c->external_symbol_list);
	return copy;
}

static Module *sema_instantiate_module(Context *context, Module *module, Path *path, Expr **parms)
{
	Module *new_module = compiler_find_or_create_module(path, NULL);
	new_module->is_generic = true;
	Context **contexts = module->contexts;
	VECEACH(contexts, i)
	{
		vec_add(new_module->contexts, copy_context(new_module, contexts[i]));
	}
	Context *first_context = new_module->contexts[0];
	VECEACH(module->parameters, i)
	{
		TokenId param = module->parameters[i];
		Decl *decl = decl_new_with_type(param, DECL_TYPEDEF, VISIBLE_PUBLIC);
		decl->resolve_status = RESOLVE_DONE;
		Expr *type_info_expr = parms[i];
		assert(type_info_expr->expr_kind == EXPR_TYPEINFO);
		assert(type_info_expr->type_expr->resolve_status == RESOLVE_DONE);
		decl->typedef_decl.type_info = type_info_expr->type_expr;
		decl->type->name = decl->name;
		decl->type->canonical = type_info_expr->type_expr->type->canonical;
		vec_add(first_context->global_decls, decl);
	}
	return new_module;
}

static Decl *sema_analyse_parameterized_define(Context *c, Decl *decl)
{
	TokenId define_ident;;
	TypeInfo *define_type;
	if (decl->define_decl.define_kind == DEFINE_TYPE_ALIAS)
	{
		define_type = decl->define_decl.type_info;
	}
	else
	{
		define_ident = decl->define_decl.identifier;
	}
	Path *decl_path;
	bool mismatch = false;
	const char *name;
	SourceSpan mismatch_span = INVALID_RANGE;
	switch (decl->define_decl.define_kind)
	{
		case DEFINE_IDENT_ALIAS:
			decl_path = decl->define_decl.path;
			name = TOKSTR(define_ident);
			break;
		case DEFINE_TYPE_ALIAS:
			mismatch_span = define_type->span;
			decl_path = define_type->unresolved.path;
			name = TOKSTR(define_type->unresolved.name_loc);
			mismatch = define_type->kind != TYPE_INFO_IDENTIFIER;
			break;
		default:
			UNREACHABLE
	}
	if (mismatch)
	{
		sema_error_range(mismatch_span, "A variable, type or constant was expected here.");
		return poisoned_decl;
	}
	if (!decl_path)
	{
		sema_error_range(mismatch_span, "Parameterization requires the full path, please add it.");
		return poisoned_decl;
	}
	Decl *import_found = NULL;
	VECEACH(c->imports, i)
	{
		Decl *import = c->imports[i];
		if (decl_path->module == import->import.path->module)
		{
			import_found = import;
			break;
		}
	}
	if (!import_found)
	{
		sema_error_range(mismatch_span, "It was not possible to find a matching module, did you use the full path?");
		return poisoned_decl;
	}

	Module *module = import_found->module;
	Expr **params = decl->define_decl.params;
	unsigned parameter_count = vec_size(module->parameters);
	assert(parameter_count > 0);
	if (parameter_count != vec_size(params))
	{
		sema_error_range((SourceSpan) { params[0]->span.loc, VECLAST(params)->span.end_loc }, "The generic module expected %d arguments, but you only supplied %d, did you make a mistake?",
		           parameter_count, vec_size(decl->define_decl.params));
		return poisoned_decl;
	}
	scratch_buffer_clear();
	scratch_buffer_append_len(module->name->module, module->name->len);
	scratch_buffer_append_char('.');
	VECEACH(decl->define_decl.params, i)
	{
		Expr *expr = decl->define_decl.params[i];
		if (expr->expr_kind != EXPR_TYPEINFO)
		{
			SEMA_ERROR(expr, "A generic module can only take plain types as arguments.");
			return poisoned_decl;
		}
		if (!sema_resolve_type_info(c, expr->type_expr)) return poisoned_decl;
		if (i != 0) scratch_buffer_append_char('.');
		const char *type_name = expr->type_expr->type->canonical->name;
		scratch_buffer_append(type_name);
	}
	TokenType ident_type = TOKEN_IDENT;
	const char *res = scratch_buffer_to_string();
	size_t len = global_context.scratch_buffer_len;
	const char *path_string = symtab_add(res, len, fnv1a(res, len), &ident_type);
	Module *instantiated_module = global_context_find_module(path_string);
	if (!instantiated_module)
	{
		Path *path = CALLOCS(Path);
		path->module = path_string;
		path->span = module->name->span;
		path->len = len;
		instantiated_module = sema_instantiate_module(c, module, path, decl->define_decl.params);
		sema_analyze_stage(instantiated_module, c->module->stage - 1);
	}
	Decl *symbol = module_find_symbol(instantiated_module, name);
	if (!symbol)
	{
		SEMA_ERROR(decl, "Identifier '%s' could not be found.", name);
		return poisoned_decl;
	}
	if (symbol->visibility <= VISIBLE_MODULE && !import_found->import.private)
	{
		SEMA_TOKID_ERROR(decl->name_token, "'%s' is not visible from this module.", symbol->name);
		return poisoned_decl;
	}
	context_register_external_symbol(c, symbol);
	return symbol;
}
static void decl_define_type(Decl *decl, Type *actual_type)
{
	Type *type = type_new(TYPE_TYPEDEF, decl->name);
	type->decl = decl;
	type->canonical = actual_type->canonical;
	decl->type = type;
}

static inline bool sema_analyse_define(Context *c, Decl *decl)
{
	Decl *symbol;
	if (decl->define_decl.is_parameterized)
	{
		symbol = TRY_DECL_OR(sema_analyse_parameterized_define(c, decl), poisoned_decl);
	}
	else
	{
		switch (decl->define_decl.define_kind)
		{
			case DEFINE_FUNC:
			{
				Type *func_type = sema_analyse_function_signature(c, &decl->define_decl.function_signature, false);
				if (!func_type) return false;
				decl_define_type(decl, type_get_ptr(func_type));
				return true;
			}
			case DEFINE_TYPE_ALIAS:
				if (!sema_resolve_type_info(c, decl->define_decl.type_info)) return false;
				decl_define_type(decl, decl->define_decl.type_info->type);
				return true;
			case DEFINE_DISTINCT_TYPE:
			{
				if (!sema_resolve_type_info(c, decl->define_decl.type_info)) return false;
				Type *type = type_new(TYPE_DISTINCT, decl->name);
				type->decl = decl;
				type->canonical = type;
				decl->type = type;
				return true;
			}
			case DEFINE_IDENT_ALIAS:
			{
				Decl *ambiguous_decl = NULL;
				Decl *private_symbol = NULL;
				symbol = sema_resolve_symbol(c,
				                             TOKSTR(decl->define_decl.identifier),
				                             decl->define_decl.path,
				                             &ambiguous_decl,
				                             &private_symbol);
				if (!symbol) TODO;
				break;
			}
		}
	}
	switch (decl->define_decl.define_kind)
	{
		case DEFINE_FUNC:
		case DEFINE_DISTINCT_TYPE:
			UNREACHABLE
		case DEFINE_TYPE_ALIAS:
			decl_define_type(decl, symbol->type);
			break;
		case DEFINE_IDENT_ALIAS:
			decl->type = symbol->type;
			decl->define_decl.alias = symbol;
			break;
	}
	return true;
}


/**
 * Semantic analysis on an error first checks the internals as if it was
 * a struct, then checks that the size is not exceeded and adds padding.
 */
static inline bool sema_analyse_error(Context *context, Decl *decl)
{
	// 1. Step one is to analyze the error as it it was a regular struct.
	if (!sema_analyse_struct_union(context, decl)) return false;

	// 2. Because an error is always pointer sized, we check so that it isn't exceeded.
	ByteSize error_full_size = type_size(type_uptr);
	if (decl->strukt.size > error_full_size)
	{
		SEMA_ERROR(decl, "Error type may not exceed pointer size (%d bytes) it was %d bytes.", error_full_size, decl->strukt.size);
		return false;
	}

	// 3. If the size is smaller than than pointer sized, we add padding.
	if (decl->strukt.size < error_full_size)
	{
		decl->strukt.padding = error_full_size - decl->strukt.size;
		decl->strukt.size = error_full_size;
	}
	return true;
}


bool sema_analyse_decl(Context *context, Decl *decl)
{
	if (decl->resolve_status == RESOLVE_DONE) return decl_ok(decl);

	DEBUG_LOG(">>> Analysing %s.", decl->name ? decl->name : "anon");
	if (decl->resolve_status == RESOLVE_RUNNING)
	{
		SEMA_ERROR(decl, "Recursive definition of '%s'.", decl->name ? decl->name : "anon");
		decl_poison(decl);
		return false;
	}

	decl->resolve_status = RESOLVE_RUNNING;
	decl->module = context->module;
	switch (decl->decl_kind)
	{
		case DECL_INTERFACE:
			TODO
		case DECL_STRUCT:
		case DECL_UNION:
			if (!sema_analyse_struct_union(context, decl)) return decl_poison(decl);
			decl_set_external_name(decl);
			break;
		case DECL_FUNC:
			if (!sema_analyse_func(context, decl)) return decl_poison(decl);
			decl_set_external_name(decl);
			break;
		case DECL_MACRO:
			if (!sema_analyse_macro(context, decl)) return decl_poison(decl);
			break;
		case DECL_VAR:
			if (!sema_analyse_global(context, decl)) return decl_poison(decl);
			decl_set_external_name(decl);
			break;
		case DECL_DISTINCT:
			if (!sema_analyse_distinct(context, decl)) return decl_poison(decl);
			break;
		case DECL_TYPEDEF:
			if (!sema_analyse_typedef(context, decl)) return decl_poison(decl);
			break;
		case DECL_ENUM:
			if (!sema_analyse_enum(context, decl)) return decl_poison(decl);
			decl_set_external_name(decl);
			break;
		case DECL_ERR:
			if (!sema_analyse_error(context, decl)) return decl_poison(decl);
			decl_set_external_name(decl);
			break;
		case DECL_GENERIC:
			if (!sema_analyse_generic(context, decl)) return decl_poison(decl);
			break;
		case DECL_DEFINE:
			if (!sema_analyse_define(context, decl)) return decl_poison(decl);
			break;
		case DECL_ATTRIBUTE:
			TODO
		case DECL_POISONED:
		case DECL_IMPORT:
		case DECL_ENUM_CONSTANT:
		case DECL_ARRAY_VALUE:
		case DECL_CT_ELSE:
		case DECL_CT_ELIF:
		case DECL_LABEL:
		case DECL_CT_SWITCH:
		case DECL_CT_CASE:
		case DECL_CT_IF:
			UNREACHABLE
	}
	decl->resolve_status = RESOLVE_DONE;
	return true;
}

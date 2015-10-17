#include "lua-open62541.h"

static UA_Boolean isbuiltin(const UA_DataType *type) {
    if(type->typeIndex > UA_TYPES_DIAGNOSTICINFO)
        return UA_FALSE;
    if(type->typeIndex == UA_TYPES_LOCALIZEDTEXT ||
       type->typeIndex == UA_TYPES_EXPANDEDNODEID ||
       type->typeIndex == UA_TYPES_QUALIFIEDNAME)
        return UA_FALSE;
    return UA_TRUE;
}

/* if the data is transfomed, replace at the original index in the stack */
ua_data * ua_getdata(lua_State *L, int index) {
    ua_data *data = luaL_testudata(L, index , "open62541-data");
    if(!data)
        data = luaL_testudata(L, index , "open62541-builtin");
    if(data)
        return data;
    /* try to convert */
    if(lua_isboolean(L, index)) {
        data = lua_newuserdata(L, sizeof(ua_data));
        data->type = &UA_TYPES[UA_TYPES_BOOLEAN];
        data->data = UA_Boolean_new();
        *(UA_Boolean*)data->data = lua_toboolean(L, index);
        luaL_setmetatable(L, "open62541-builtin");
        lua_replace(L, index);
    } else if(lua_isnumber(L, index)) {
        data = lua_newuserdata(L, sizeof(ua_data));
        data->type = &UA_TYPES[UA_TYPES_INT32];
        data->data = UA_Int32_new();
        *(UA_Int32*)data->data = lua_tonumber(L, index);
        luaL_setmetatable(L, "open62541-builtin");
        lua_replace(L, index);
    } else if(lua_isstring(L, index)) {
        data = lua_newuserdata(L, sizeof(ua_data));
        data->type = &UA_TYPES[UA_TYPES_STRING];
        data->data = UA_String_new();
        *(UA_String*)data->data = UA_STRING_ALLOC(lua_tostring(L, index));
        luaL_setmetatable(L, "open62541-builtin");
        lua_replace(L, index);
    }
    return data;
}

/* returns an array
   - if the index points to one
   - if its a lua array (table with increasing numeric entries)
   and all the types match (we set type == null for the empty array) */
ua_array * ua_getarray(lua_State *L, int index) {
    ua_array *array = luaL_testudata(L, index , "open62541-array");
    if(array || !lua_istable(L, index))
        return array;

    /* we have a lua array. transform it. */
    array = lua_newuserdata(L, sizeof(ua_array));
    array->local_data = NULL;
    array->local_length = -1;
    array->type = NULL;
    array->data = &array->local_data;
    array->length = &array->local_length;
    luaL_setmetatable(L, "open62541-array");

    size_t len = lua_rawlen(L, index);
    if(len == 0)
        return array;

    /* get the first entry */
    lua_rawgeti(L, index, 1);
    ua_data *data = ua_getdata(L, -1);
    if(!data) {
        lua_pop(L, 2);
        luaL_error(L, "cannot transform to an open62541 array");
        return NULL;
    }
    array->local_data = malloc(len * data->type->memSize);
    UA_copy(data->data, array->local_data, data->type);
    array->local_length = 1;
    array->type = data->type;
    lua_pop(L, 1);
    for(size_t i = 2; i <= len; i++) {
        lua_rawgeti(L, index, i);
        data = ua_getdata(L, -1);
        if(!data || data->type != array->type) {
            lua_pop(L, 2);
            UA_Array_delete(array->local_data, array->type, array->local_length);
            array->local_length = -1;
            array->local_data = NULL;
            array->type = NULL;
            return NULL;
        }
        UA_copy(data->data, array->local_data + (data->type->memSize * array->local_length), data->type);
        array->local_length++;
        lua_pop(L, 1);
    }
    lua_replace(L, index); // move the new ua_array to the inde of the lua-array we copied
    return array;
}

/* Guid */
static UA_Guid parse_guid(lua_State *L, int index) {
    UA_Guid guid;
    UA_Guid_init(&guid);
    const char *arg = NULL;
    if(index > 0) {
        size_t arglength;
        if(!lua_isstring(L, index))
            luaL_error(L, "Guid needs to be of the form \"00000000-0000-0000-0000-000000000000\"");
        else { 
            arg = lua_tolstring(L, index, &arglength);
            if(arglength != 36) {
                luaL_error(L, "Guid needs to be of the form \"00000000-0000-0000-0000-000000000000\"");
                arg = NULL;
            }
        }
    }
    if(arg) {
        guid.data1 = strtoull(arg, NULL, 16);
        guid.data2 = strtoull(&arg[9], NULL, 16);
        guid.data3 = strtoull(&arg[14], NULL, 16);
        UA_Int16 data4_1 = strtoull(&arg[19], NULL, 16);
        guid.data4[0] = data4_1 >> 8;
        guid.data4[1] = data4_1;
        UA_Int64 data4_2 = strtoull(&arg[24], NULL, 16);
        guid.data4[2] = data4_2 >> 40;
        guid.data4[3] = data4_2 >> 32;
        guid.data4[4] = data4_2 >> 24;
        guid.data4[5] = data4_2 >> 16;
        guid.data4[6] = data4_2 >> 8;
        guid.data4[7] = data4_2;
    }
    return guid;
}

/* NodeId */
static UA_NodeId parse_nodeid(lua_State *L, int index) {
    UA_NodeId id;
    UA_NodeId_init(&id);
    if(lua_isnil(L, index))
        return id;
    if(!lua_isnumber(L, index))
        luaL_error(L, "The first argument is no integer namespace id");
    else if(!lua_isnumber(L, index+1) || !lua_isstring(L, index+1))
        luaL_error(L, "Currently, only numeric and string nodeids are implemented");
    else {
        id.namespaceIndex = lua_tointeger(L, index);
        if(lua_isnumber(L, index+1)) {
            id.identifierType = UA_NODEIDTYPE_NUMERIC;
            id.identifier.numeric = lua_tointeger(L, index+1);
        } else if(lua_isstring(L, index+1)) {
            id.identifierType = UA_NODEIDTYPE_STRING;
            id.identifier.string = UA_String_fromChars(lua_tostring(L, 2));
        } 
    }
    return id;
}

static void ua_nodeid_tostring(lua_State *L, const UA_NodeId *id) {
    if(id->identifierType == UA_NODEIDTYPE_NUMERIC)
        lua_pushfstring(L, "nodeid(ns=%d,i=%d)", id->namespaceIndex, id->identifier.numeric);
    else if(id->identifierType == UA_NODEIDTYPE_STRING) {
        char out[512];
        snprintf(out, 512, "nodeid(ns=%i,s=%.*s)", id->namespaceIndex,
                 id->identifier.string.length, id->identifier.string.data);
        lua_pushstring(L, out);
    } else
        lua_pushstring(L, "Unknown NodeId type");
}

/* QualifiedName */
static UA_QualifiedName parse_qualifiedname(lua_State *L, int index) {
    UA_QualifiedName qn;
    UA_QualifiedName_init(&qn);
    if(lua_isnil(L, index))
        return qn;
    if(!lua_isnumber(L, index))
        luaL_error(L, "The first argument is no integer namespace index");
    else if(!lua_isstring(L, index+1))
        luaL_error(L, "The second argument is no string");
    else {
        qn.namespaceIndex = lua_tointeger(L, index);
        qn.name = UA_STRING_ALLOC(lua_tostring(L, index+1));
    }
    return qn;
}

/* LocalizedText */
static UA_LocalizedText parse_localizedtext(lua_State *L, int index) {
    UA_LocalizedText lt;
    UA_LocalizedText_init(&lt);
    if(lua_isnil(L, index))
        return lt;
    if(!lua_isstring(L, index))
            luaL_error(L, "The first argument is no string");
    else if(!lua_isstring(L, index+1))
            luaL_error(L, "The second argument is no string");
    else {
        lt.locale = UA_STRING_ALLOC(lua_tostring(L, index));
        lt.text = UA_STRING_ALLOC(lua_tostring(L, index+1));
    }
    return lt;
}

/* Variant */
static void
ua_variant_set(lua_State *L, UA_Variant *v, int index) {
    ua_data *data = luaL_checkudata (L, index, "open62541-builtin");
    if(data) {
        UA_Variant_deleteMembers(v);
        UA_Variant_setScalarCopy(v, data->data, data->type);
        return;
    }
    
    ua_array *array = luaL_checkudata (L, index, "open62541-array");
    if(array) {
        UA_Variant_deleteMembers(v);
        v->type = array->type;
        if(!*array->data)
            return;
        if(*array->length < 0)
            UA_Variant_setScalarCopy(v, *array->data, array->type);
        else
            UA_Variant_setArrayCopy(v, *array->data, *array->length, array->type);
        // todo: array dimensions
        return;
    }
    luaL_error(L, "Only open62541 types can be set to a variant");
    return;
}

static void
pushlower(lua_State *L, const char *str) {
    char out[512];
    for(int i = 0; i<512; i++) {
        out[i] = tolower(str[i]);
        if(!str[i])
            break;
    }
    lua_pushstring(L, out);
}

static int
ua_new_type_closure(lua_State *L) {
    lua_pushnil(L); // separator between data from the user and what we add
    UA_DataType *type = lua_touserdata(L, lua_upvalueindex(1));
    ua_data *data = lua_newuserdata(L, sizeof(ua_data));
    data->type = type;
    data->data = UA_new(type);
    if(isbuiltin(type))
        luaL_setmetatable(L, "open62541-builtin");
    else
        luaL_setmetatable(L, "open62541-data");

    if(type == &UA_TYPES[UA_TYPES_STRING]) {
        if(lua_isnil(L, 1))
            *(UA_String*)data->data = UA_STRING_NULL;   
        else if(!lua_isstring(L, 1))
            return luaL_error(L, "Argument is not a string");
        else
            *(UA_String*)data->data = UA_STRING_ALLOC(lua_tostring(L, 1));
    }
    if(type == &UA_TYPES[UA_TYPES_GUID])
        *(UA_Guid*)data->data = parse_guid(L, 1);
    if(type == &UA_TYPES[UA_TYPES_NODEID])
        *(UA_NodeId*)data->data = parse_nodeid(L, 1);
    if(type == &UA_TYPES[UA_TYPES_QUALIFIEDNAME])
        *(UA_QualifiedName*)data->data = parse_qualifiedname(L, 1);
    if(type == &UA_TYPES[UA_TYPES_LOCALIZEDTEXT])
        *(UA_LocalizedText*)data->data = parse_localizedtext(L, 1);
    return 1;
}

static int
find_memberindex(const UA_DataType *type, const char *membername) {
    int memberindex;
    int length = strlen(membername);
    for(memberindex = 0; memberindex < type->membersSize; memberindex++) {
        UA_Boolean found = UA_TRUE;
        const UA_DataTypeMember *member = &type->members[memberindex];
        if(!member->memberName)
            continue;
        if(strlen(member->memberName) != length)
            continue;
        for(size_t i = 0; i < length; i++) {
            if(tolower(member->memberName[i]) != membername[i]) {
                found = UA_FALSE;
                break;
            }
        }
        if(found)
            return memberindex;
    }
    return -1;
}

/* get the pointer to the member and/or the array length */
static void *
memberptr(void *parent, const UA_DataType *type, int memberindex, UA_Int32 **arraylen) {
    void *ptr = parent;
    const UA_DataTypeMember *member;
    for(int i = 0; i < memberindex; i++) {
        member = &type->members[i];
        if(!member->isArray)
            ptr += member->padding + UA_TYPES[member->memberTypeIndex].memSize;
        else
            ptr += (member->padding >> 3) + sizeof(UA_Int32) +
                (member->padding & 0x07) + sizeof(void*);
    }
    member = &type->members[memberindex];
    if(member->isArray) {
        ptr += (member->padding >> 3);
        *arraylen = (UA_Int32*)ptr;
        return ptr + sizeof(UA_Int32) + (member->padding & 0x07);
    }
    *arraylen = NULL;
    return ptr + member->padding;
}

int ua_gc(lua_State *L) {
    ua_data *data = ua_getdata(L, -1);
    if(!data)
        return 0;
    lua_getuservalue(L, -1);
    /* derived data is not deleted, tagged with a uservalue */
    if(lua_isnil(L, -1))
        UA_delete(data->data, data->type);
    return 0;
}

static int
ua_getmemberindex(lua_State *L, ua_data *data, int memberindex) {
    UA_Int32 *arraylen;
    void *member = memberptr(data->data, data->type, memberindex, &arraylen);
    if(!arraylen) {
        /* scalar type */
        ua_data *memberdata = lua_newuserdata(L, sizeof(ua_data));
        memberdata->type = &UA_TYPES[data->type->members[memberindex].memberTypeIndex];
        memberdata->data = member;
        if(isbuiltin(memberdata->type))
            luaL_setmetatable(L, "open62541-builtin");
        else
            luaL_setmetatable(L, "open62541-data");
    } else {
        /* array type */
        ua_array *memberdata = lua_newuserdata(L, sizeof(ua_array));
        memberdata->type = &UA_TYPES[data->type->members[memberindex].memberTypeIndex];
        memberdata->data = (void**)member;
        memberdata->length = arraylen;
        luaL_setmetatable(L, "open62541-array");
    }
    
    /* add a user value that prevents gc of the orig userdata */
    lua_newtable(L);
    lua_pushvalue(L, 1); // orig userdata
    lua_rawseti(L, -2, 1);
    lua_setuservalue(L, -2); // set the uservalue of the member data
    return 1;
}

int ua_index(lua_State *L) {
    ua_data *data = luaL_checkudata (L, 1, "open62541-data");
    if(!lua_isstring(L, 2))
        return luaL_error(L, "Unknown index");
    int memberindex = find_memberindex(data->type, lua_tostring(L, 2));
    if(memberindex < 0) {
        lua_pushnil(L);
        return 1;
    }
    return ua_getmemberindex(L, data, memberindex);
}

int ua_newindex(lua_State *L) {
    ua_data *parent = luaL_checkudata (L, 1, "open62541-data");
    if(!lua_isstring(L, 2))
        return luaL_error(L, "Index must be a string");
    int memberindex = find_memberindex(parent->type, lua_tostring(L, 2));
    if(memberindex < 0)
        return luaL_error(L, "Index not found");
    const UA_DataType *membertype = &UA_TYPES[parent->type->members[memberindex].memberTypeIndex];
    UA_Int32 *arraylen;
    void *member = memberptr(parent->data, parent->type, memberindex, &arraylen);
    if(!arraylen && membertype == &UA_TYPES[UA_TYPES_VARIANT]) {
        ua_data *data = ua_getdata(L, 3);
        if(data) {
            UA_Variant_deleteMembers(member);
            UA_Variant_setScalarCopy(member, data->data, data->type);
            return 0;
        }
        ua_array *array = ua_getarray(L, 3);
        if(array) {
            UA_Variant_deleteMembers(member);
            UA_Variant_setArrayCopy(member, *array->data, *array->length, array->type);
            return 0;
        }
        return luaL_error(L, "Could not convert to an open62541 type");
    }
    if(!arraylen) {
        ua_data *data = ua_getdata(L, 3);
        if(!data)
            return luaL_error(L, "Could not convert to an open62541 type");
        if(data->type == &UA_TYPES[UA_TYPES_INT32]) {
            UA_Int32 n = *(UA_Int32*)data->data;
            switch(membertype->typeIndex + (0x8000 * !membertype->namespaceZero)) {
            case UA_TYPES_BOOLEAN:
                *(UA_Boolean*)member = n;
                    return 0;
            case UA_TYPES_SBYTE:
            case UA_TYPES_BYTE:
                *(UA_Byte*)member = n;
                return 0;
            case UA_TYPES_INT16:
            case UA_TYPES_UINT16:
                *(UA_Int16*)member = n;
                return 0;
            case UA_TYPES_INT32:
            case UA_TYPES_UINT32:
            case UA_TYPES_STATUSCODE:
                *(UA_Int32*)member = n;
                return 0;
            case UA_TYPES_INT64:
            case UA_TYPES_UINT64:
            case UA_TYPES_DATETIME:
                *(UA_Int64*)member = n;
                return 0;
            }
        }
        if(membertype != data->type)
            return luaL_error(L, "Types don't match");
        UA_deleteMembers(member, membertype);
        UA_copy(data->data, member, membertype);
    } else {
        ua_array *array = ua_getarray(L, 3);
        if(!array)
            return luaL_error(L, "Could not convert to an open62541 array");
        if(membertype != array->type)
            return luaL_error(L, "Types don't match");
        UA_Array_delete(*(void**)member, membertype, *arraylen);
        UA_Array_copy(member, *array->data, membertype, *array->length);
        *arraylen = *array->length;
    }
    return 0;
}

static int
ua_iterate(lua_State *L) {
    ua_data *data = luaL_checkudata (L, 1, "open62541-data");
    int memberindex;
    if(lua_isnil(L, 2))
        memberindex = 0;
    else if(lua_isstring(L, 2)) {
        memberindex = find_memberindex(data->type, lua_tostring(L, 2));
        if(memberindex < 0)
            return luaL_error(L, "No valid index");
        memberindex++;
    } else
       return luaL_error(L, "Can only iterate over string indices");
    if(memberindex >= data->type->membersSize)
        return 0;
    pushlower(L, data->type->members[memberindex].memberName);
    ua_getmemberindex(L, data, memberindex);
    return 2;
}
        
int ua_pairs(lua_State *L) {
    ua_data *data = luaL_checkudata (L, 1, "open62541-data");
    lua_pushcfunction(L, ua_iterate);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

int ua_tostring(lua_State *L) {
    ua_data *data = luaL_checkudata (L, -1, "open62541-builtin");
    char out[37];
    switch(data->type->typeIndex) {
    case UA_TYPES_BOOLEAN:
        if(*(UA_Boolean*)data->data)
            lua_pushstring(L, "true");
        else
            lua_pushstring(L, "false");
        return 1;
    case UA_TYPES_SBYTE:
        snprintf(out, 40, "%hhi", *(UA_SByte*)data->data);
        lua_pushstring(L, out);
        return 1;
    case UA_TYPES_BYTE:
        snprintf(out, 40, "%hhu", *(UA_Byte*)data->data);
        lua_pushstring(L, out);
        return 1;
    case UA_TYPES_INT16:
        snprintf(out, 40, "%hi", *(UA_Int16*)data->data);
        lua_pushstring(L, out);
        return 1;
    case UA_TYPES_UINT16:
        snprintf(out, 40, "%hu", *(UA_UInt16*)data->data);
        lua_pushstring(L, out);
        return 1;
    case UA_TYPES_INT32:
        snprintf(out, 40, "%i", *(UA_Int32*)data->data);
        lua_pushstring(L, out);
        return 1;
    case UA_TYPES_UINT32:
        snprintf(out, 40, "%u", *(UA_UInt32*)data->data);
        lua_pushstring(L, out);
        return 1;
    case UA_TYPES_INT64:
        snprintf(out, 40, "%li", *(UA_Int64*)data->data);
        lua_pushstring(L, out);
        return 1;
    case UA_TYPES_UINT64:
        snprintf(out, 40, "%lu", *(UA_UInt64*)data->data);
        lua_pushstring(L, out);
        return 1;
    case UA_TYPES_STATUSCODE:
        snprintf(out, 40, "0x%08x", *(UA_StatusCode*)data->data);
        lua_pushstring(L, out);
        return 1;
    case UA_TYPES_FLOAT:
        snprintf(out, 40, "%f", *(UA_Float*)data->data);
        lua_pushstring(L, out);
        return 1;
    case UA_TYPES_DOUBLE:
        snprintf(out, 40, "%lf", *(UA_Double*)data->data);
        lua_pushstring(L, out);
        return 1;
    case UA_TYPES_DATETIME:
        lua_pushstring(L, data->type->typeName);
        return 1;
    case UA_TYPES_GUID: {
        UA_Guid *id = (UA_Guid*)data->data;
        snprintf(out, 37, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 id->data1, id->data2, id->data3, id->data4[0], id->data4[1],
                 id->data4[2], id->data4[3], id->data4[4], id->data4[5], id->data4[6], id->data4[7]);
        lua_pushstring(L, out);
        return 1;
    }
    case UA_TYPES_STRING:
    case UA_TYPES_BYTESTRING:
    case UA_TYPES_XMLELEMENT: {
        UA_String *s = (UA_String*)data->data;
        if(s->length <= 0)
            lua_pushlstring(L, NULL, 0);
        else
            lua_pushlstring(L, s->data, s->length);
        return 1;
    }
    case UA_TYPES_NODEID:
        ua_nodeid_tostring(L, (UA_NodeId*)data->data);
        return 1;
    case UA_TYPES_EXTENSIONOBJECT:
        lua_pushstring(L, data->type->typeName);
        return 1;
    case UA_TYPES_DATAVALUE:
        lua_pushstring(L, data->type->typeName);
        return 1;
    case UA_TYPES_VARIANT:
        lua_pushstring(L, data->type->typeName);
        return 1;
    case UA_TYPES_DIAGNOSTICINFO:
        lua_pushstring(L, data->type->typeName);
        return 1;
    }

    lua_pushstring(L, data->type->typeName);
    return 1;
}

int ua_array_index(lua_State *L) {
    ua_array *array = luaL_checkudata (L, 1, "open62541-array");
    if(lua_isstring(L, 2)) {
        const char *index = lua_tostring(L, 2);
        if(strcmp(index, "append") == 0) {
            lua_pushcfunction(L, ua_array_append);
            return 1;
        }
    }
    if(!lua_isnumber(L, 2))
        return luaL_error(L, "Unknown index");

    int index = lua_tointeger(L, 2);
    index--; // lua is 1-indexed
    if(index < 0 || index > *array->length)
        return luaL_error(L, "Index out of range");

    ua_data *data = lua_newuserdata(L, sizeof(ua_data));
    data->type = array->type;
    data->data = UA_new(array->type);
    UA_copy(*array->data + (index * array->type->memSize), data->data, array->type);
    if(isbuiltin(array->type))
        luaL_setmetatable(L, "open62541-builtin");
    else
        luaL_setmetatable(L, "open62541-data");
    return 1;
}

int ua_array_append(lua_State *L) {
    ua_array *array = luaL_checkudata (L, 1, "open62541-array");
    ua_data *value = luaL_checkudata (L, 2, "open62541-data");
    if(array->type != value->type)
        return luaL_error(L, "Types don't match");
    UA_Int32 newlength = (*array->length) + 1;
    if(newlength <= 0)
        newlength = 1;
    *array->data = realloc(*array->data, newlength * array->type->memSize);
    UA_copy(value->data, *array->data + ((newlength-1) * array->type->memSize), array->type);
    *(array->length) = newlength;
    return 0;
}

int ua_array_len(lua_State *L) {
    ua_array *array = luaL_checkudata (L, 1, "open62541-array");
    if(*array->length <= 0)
        lua_pushinteger(L, 0);
    else
        lua_pushinteger(L, *array->length);
    return 1;
}

static int
ua_array_iterate(lua_State *L) {
    ua_array *array = luaL_checkudata (L, 1, "open62541-array");
    if(!array)
        return luaL_error(L, "Not a open62541 datatype");
    int index;
    if(lua_isnil(L, 2))
        index = 0;
    else if(lua_isnumber(L, 2))
        index = lua_tonumber(L, 2);
    else
        return luaL_error(L, "Can only iterate over numeric indices");
    if(index < 0 || index >= *array->length)
        return 0;
    lua_pushnumber(L, index+1);
    ua_data *data = lua_newuserdata(L, sizeof(ua_data));
    data->type = array->type;
    data->data = UA_new(array->type);
    UA_copy(*array->data + (index * array->type->memSize), data->data, array->type);
    luaL_setmetatable(L, "open62541-data");
    return 2;
}
        
int ua_array_pairs(lua_State *L) {
    ua_array *array = luaL_checkudata (L, 1, "open62541-array");
    if(!array)
        return luaL_error(L, "Not a open62541 datatype");
    lua_pushcfunction(L, ua_array_iterate);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

void ua_populate_types(lua_State *L) {
    lua_newtable(L);
    for(int i = 0; i < UA_TYPES_COUNT; i++) {
        lua_pushstring(L, UA_TYPES[i].typeName);
        lua_pushlightuserdata(L, (void*)&UA_TYPES[i]);
        lua_pushcclosure(L, &ua_new_type_closure, 1);
        lua_settable(L, -3);
    }
}

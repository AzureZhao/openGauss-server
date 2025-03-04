/* -------------------------------------------------------------------------
 *
 * tsearchcmds.cpp
 *
 *	  Routines for tsearch manipulation commands
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/optimizer/commands/tsearchcmds.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include <ctype.h>

#include "access/heapam.h"
#include "access/tableam.h"
#include "access/genam.h"
#include "access/reloptions.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_config_map.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "securec.h"
#include "tsearch/ts_cache.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rel_gs.h"
#include "utils/syscache.h"
#include "utils/snapmgr.h"

static void MakeConfigurationMapping(AlterTSConfigurationStmt* stmt, HeapTuple tup, Relation relMap);
static void DropConfigurationMapping(AlterTSConfigurationStmt* stmt, HeapTuple tup, Relation relMap);
static void SetConfigurationOptions(AlterTSConfigurationStmt* stmt, HeapTuple tup);

/* --------------------- TS Parser commands ------------------------ */
/*
 * lookup a parser support function and return its OID (as a Datum)
 *
 * attnum is the pg_ts_parser column the function will go into
 */
static Datum get_ts_parser_func(DefElem* defel, int attnum)
{
    List* funcName = defGetQualifiedName(defel);
    Oid typeId[3];
    Oid retTypeId;
    int nargs;
    Oid procOid;

    retTypeId = INTERNALOID; /* correct for most */
    typeId[0] = INTERNALOID;
    switch (attnum) {
        case Anum_pg_ts_parser_prsstart:
            nargs = 2;
            typeId[1] = INT4OID;
            break;
        case Anum_pg_ts_parser_prstoken:
            nargs = 3;
            typeId[1] = INTERNALOID;
            typeId[2] = INTERNALOID;
            break;
        case Anum_pg_ts_parser_prsend:
            nargs = 1;
            retTypeId = VOIDOID;
            break;
        case Anum_pg_ts_parser_prsheadline:
            nargs = 3;
            typeId[1] = INTERNALOID;
            typeId[2] = TSQUERYOID;
            break;
        case Anum_pg_ts_parser_prslextype:
            nargs = 1;

            /*
             * Note: because the lextype method returns type internal, it must
             * have an internal-type argument for security reasons.  The
             * argument is not actually used, but is just passed as a zero.
             */
            break;
        default:
            /* should not be here */
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized attribute for text search parser: %d", attnum)));
            nargs = 0; /* keep compiler quiet */
    }

    procOid = LookupFuncName(funcName, nargs, typeId, false);
    if (get_func_rettype(procOid) != retTypeId)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                errmsg("function %s should return type %s",
                    func_signature_string(funcName, nargs, NIL, typeId),
                    format_type_be(retTypeId))));

    return ObjectIdGetDatum(procOid);
}

/*
 * make pg_depend entries for a new pg_ts_parser entry
 */
static void makeParserDependencies(HeapTuple tuple)
{
    Form_pg_ts_parser prs = (Form_pg_ts_parser)GETSTRUCT(tuple);
    ObjectAddress myself, referenced;

    myself.classId = TSParserRelationId;
    myself.objectId = HeapTupleGetOid(tuple);
    myself.objectSubId = 0;

    /* dependency on namespace */
    referenced.classId = NamespaceRelationId;
    referenced.objectId = prs->prsnamespace;
    referenced.objectSubId = 0;
    recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

    /* dependency on extension */
    recordDependencyOnCurrentExtension(&myself, false);

    /* dependencies on functions */
    referenced.classId = ProcedureRelationId;
    referenced.objectSubId = 0;

    referenced.objectId = prs->prsstart;
    recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

    referenced.objectId = prs->prstoken;
    recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

    referenced.objectId = prs->prsend;
    recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

    referenced.objectId = prs->prslextype;
    recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

    if (OidIsValid(prs->prsheadline)) {
        referenced.objectId = prs->prsheadline;
        recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
    }
}

/*
 * CREATE TEXT SEARCH PARSER
 */
void DefineTSParser(List* names, List* parameters)
{
    char* prsname = NULL;
    ListCell* pl = NULL;
    Relation prsRel;
    HeapTuple tup;
    Datum values[Natts_pg_ts_parser];
    bool nulls[Natts_pg_ts_parser];
    NameData pname;
    Oid prsOid;
    Oid namespaceoid;

    if (!superuser())
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be system admin to create text search parsers")));

    /* Convert list of names to a name and namespace */
    namespaceoid = QualifiedNameGetCreationNamespace(names, &prsname);

    /* initialize tuple fields with name/namespace */
    errno_t rc = memset_s(values, sizeof(values), 0, sizeof(values));
    securec_check(rc, "", "");
    rc = memset_s(nulls, sizeof(nulls), false, sizeof(nulls));
    securec_check(rc, "", "");

    (void)namestrcpy(&pname, prsname);
    values[Anum_pg_ts_parser_prsname - 1] = NameGetDatum(&pname);
    values[Anum_pg_ts_parser_prsnamespace - 1] = ObjectIdGetDatum(namespaceoid);

    /*
     * loop over the definition list and extract the information we need.
     */
    foreach (pl, parameters) {
        DefElem* defel = (DefElem*)lfirst(pl);

        if (pg_strcasecmp(defel->defname, "start") == 0) {
            values[Anum_pg_ts_parser_prsstart - 1] = get_ts_parser_func(defel, Anum_pg_ts_parser_prsstart);
        } else if (pg_strcasecmp(defel->defname, "gettoken") == 0) {
            values[Anum_pg_ts_parser_prstoken - 1] = get_ts_parser_func(defel, Anum_pg_ts_parser_prstoken);
        } else if (pg_strcasecmp(defel->defname, "end") == 0) {
            values[Anum_pg_ts_parser_prsend - 1] = get_ts_parser_func(defel, Anum_pg_ts_parser_prsend);
        } else if (pg_strcasecmp(defel->defname, "headline") == 0) {
            values[Anum_pg_ts_parser_prsheadline - 1] = get_ts_parser_func(defel, Anum_pg_ts_parser_prsheadline);
        } else if (pg_strcasecmp(defel->defname, "lextypes") == 0) {
            values[Anum_pg_ts_parser_prslextype - 1] = get_ts_parser_func(defel, Anum_pg_ts_parser_prslextype);
        } else
            ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg("text search parser parameter \"%s\" not recognized", defel->defname)));
    }

    /*
     * Validation
     */
    if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_parser_prsstart - 1])))
        ereport(
            ERROR, (errcode(ERRCODE_INVALID_OBJECT_DEFINITION), errmsg("text search parser start method is required")));

    if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_parser_prstoken - 1])))
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION), errmsg("text search parser gettoken method is required")));

    if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_parser_prsend - 1])))
        ereport(
            ERROR, (errcode(ERRCODE_INVALID_OBJECT_DEFINITION), errmsg("text search parser end method is required")));

    if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_parser_prslextype - 1])))
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION), errmsg("text search parser lextypes method is required")));

    /*
     * Looks good, insert
     */
    prsRel = heap_open(TSParserRelationId, RowExclusiveLock);

    tup = heap_form_tuple(prsRel->rd_att, values, nulls);

    prsOid = simple_heap_insert(prsRel, tup);

    CatalogUpdateIndexes(prsRel, tup);

    makeParserDependencies(tup);

    /* Post creation hook for new text search parser */
    InvokeObjectAccessHook(OAT_POST_CREATE, TSParserRelationId, prsOid, 0, NULL);

    tableam_tops_free_tuple(tup);

    heap_close(prsRel, RowExclusiveLock);
}

/*
 * Guts of TS parser deletion.
 */
void RemoveTSParserById(Oid prsId)
{
    Relation relation;
    HeapTuple tup;

    relation = heap_open(TSParserRelationId, RowExclusiveLock);

    tup = SearchSysCache1(TSPARSEROID, ObjectIdGetDatum(prsId));

    if (!HeapTupleIsValid(tup))
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for text search parser %u", prsId)));

    simple_heap_delete(relation, &tup->t_self);

    ReleaseSysCache(tup);

    heap_close(relation, RowExclusiveLock);
}

/*
 * ALTER TEXT SEARCH PARSER RENAME
 */
void RenameTSParser(List* oldname, const char* newname)
{
    HeapTuple tup;
    Relation rel;
    Oid prsId;
    Oid namespaceOid;

    if (!superuser())
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be system admin to rename text search parsers")));

    rel = heap_open(TSParserRelationId, RowExclusiveLock);
    prsId = get_ts_parser_oid(oldname, false);
    tup = SearchSysCacheCopy1(TSPARSEROID, ObjectIdGetDatum(prsId));

    if (!HeapTupleIsValid(tup)) /* should not happen */
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for text search parser %u", prsId)));
    namespaceOid = ((Form_pg_ts_parser)GETSTRUCT(tup))->prsnamespace;
    if (SearchSysCacheExists2(TSPARSERNAMENSP, PointerGetDatum(newname), ObjectIdGetDatum(namespaceOid)))
        ereport(
            ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("text search parser \"%s\" already exists", newname)));

    (void)namestrcpy(&(((Form_pg_ts_parser)GETSTRUCT(tup))->prsname), newname);
    simple_heap_update(rel, &tup->t_self, tup);
    CatalogUpdateIndexes(rel, tup);

    heap_close(rel, NoLock);
    tableam_tops_free_tuple(tup);
}

/*
 * ALTER TEXT SEARCH PARSER any_name SET SCHEMA name
 */
void AlterTSParserNamespace(List* name, const char* newschema)
{
    Oid prsId, nspOid;
    Relation rel;

    rel = heap_open(TSParserRelationId, RowExclusiveLock);

    prsId = get_ts_parser_oid(name, false);

    /* get schema OID */
    nspOid = LookupCreationNamespace(newschema);

    (void)AlterObjectNamespace(rel,
        TSPARSEROID,
        TSPARSERNAMENSP,
        prsId,
        nspOid,
        Anum_pg_ts_parser_prsname,
        Anum_pg_ts_parser_prsnamespace,
        -1,
        (AclObjectKind)-1);

    heap_close(rel, RowExclusiveLock);
}

Oid AlterTSParserNamespace_oid(Oid prsId, Oid newNspOid)
{
    Oid oldNspOid;
    Relation rel;

    rel = heap_open(TSParserRelationId, RowExclusiveLock);

    oldNspOid = AlterObjectNamespace(rel,
        TSPARSEROID,
        TSPARSERNAMENSP,
        prsId,
        newNspOid,
        Anum_pg_ts_parser_prsname,
        Anum_pg_ts_parser_prsnamespace,
        -1,
        (AclObjectKind)-1);

    heap_close(rel, RowExclusiveLock);

    return oldNspOid;
}

/* ---------------------- TS Dictionary commands ----------------------- */
/*
 * make pg_depend entries for a new pg_ts_dict entry
 */
static void makeDictionaryDependencies(HeapTuple tuple, const char* dictPrefix)
{
    Form_pg_ts_dict dict = (Form_pg_ts_dict)GETSTRUCT(tuple);
    ObjectAddress myself, referenced;

    myself.classId = TSDictionaryRelationId;
    myself.objectId = HeapTupleGetOid(tuple);
    myself.objectSubId = 0;

    /* dependency on namespace */
    referenced.classId = NamespaceRelationId;
    referenced.objectId = dict->dictnamespace;
    referenced.objectSubId = 0;
    recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

    /* dependency on owner */
    if (dictPrefix != NULL) {
        /* store internal dictionary file info for drop */
        StringInfoData objfile;
        initStringInfo(&objfile);
        appendStringInfo(&objfile, "%s%c%u", dictPrefix, DICT_SEPARATOR, dict->dicttemplate);

        recordDependencyOnOwner(myself.classId, myself.objectId, dict->dictowner, objfile.data);

        pfree_ext(objfile.data);
    } else
        recordDependencyOnOwner(myself.classId, myself.objectId, dict->dictowner);

    /* dependency on extension */
    recordDependencyOnCurrentExtension(&myself, false);

    /* dependency on template */
    referenced.classId = TSTemplateRelationId;
    referenced.objectId = dict->dicttemplate;
    referenced.objectSubId = 0;
    recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
}

/*
 * verify that a template's init method accepts a proposed option list
 *
 * @in tmplId: text search template oid.
 * @in dictOptions: options to varify.
 * @in oldNum: first N options is old corresponding with dictOptions.
 * @in dictPrefix: internal dictionary file prefix.
 * @out: user dictionary files.
 */
static List* verify_dictoptions(Oid tmplId, List* dictOptions, int oldNum, const char* dictPrefix)
{
    HeapTuple tup;
    Form_pg_ts_template tform;
    Oid initmethod;
    List* filenames = NIL;

    /*
     * Suppress this test when running in a standalone backend.  This is a
     * hack to allow initdb to create prefab dictionaries that might not
     * actually be usable in template1's encoding (due to using external files
     * that can't be translated into template1's encoding).  We want to create
     * them anyway, since they might be usable later in other databases.
     */
    if (!IsUnderPostmaster || dictPrefix == NULL)
        return NIL;

    tup = SearchSysCache1(TSTEMPLATEOID, ObjectIdGetDatum(tmplId));
    if (!HeapTupleIsValid(tup)) /* should not happen */
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for text search template %u", tmplId)));
    tform = (Form_pg_ts_template)GETSTRUCT(tup);

    initmethod = tform->tmplinit;

    if (!OidIsValid(initmethod)) {
        /* If there is no init method, disallow any options */
        if (dictOptions != NULL)
            ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg("text search template \"%s\" does not accept options", NameStr(tform->tmplname))));
    } else {
        /*
         * We will change the options in init method, by replaceing each dict
         * file names with dictPrefix value.
         *
         * Call the init method and see if it complains.  We don't worry about
         * it leaking memory, since our command will soon be over anyway.
         */
        filenames = (List*)DatumGetPointer(OidFunctionCall3(
            initmethod, PointerGetDatum(dictOptions), PointerGetDatum(dictPrefix), Int32GetDatum(oldNum)));
    }

    ReleaseSysCache(tup);
    return filenames;
}

/*
 * @Description: Copy user dictionary files to local and remote.
 * @in userfiles: user dictionary file list.
 * @in dictprefix: internal dictionary file prefix.
 */
void copy_tsfiles(List* userFiles, const char* dictPrefix)
{
    /* Get postfix list */
    List* postfixes = get_tsfile_postfix(userFiles, '.');

    /* Copy to local */
    List* internalFiles = copy_tsfile_to_local(userFiles, postfixes, dictPrefix);

    /* Copy to remote/backup */
    if (IS_PGXC_COORDINATOR && !IsConnFromCoord()) {
        copy_tsfile_to_remote(internalFiles, postfixes);
    } else if (t_thrd.postmaster_cxt.ReplConnArray[1] != NULL) {
        copy_tsfile_to_backup(internalFiles);
    }

    /* Clean up */
    list_free(postfixes);
    list_free_deep(internalFiles);
}

/*
 * CREATE TEXT SEARCH DICTIONARY
 */
void DefineTSDictionary(List* names, List* parameters)
{
    ListCell* pl = NULL;
    Relation dictRel;
    HeapTuple tup;
    Datum values[Natts_pg_ts_dict];
    bool nulls[Natts_pg_ts_dict];
    NameData dname;
    Oid templId = InvalidOid;
    List* dictoptions = NIL;
    Oid dictOid;
    Oid namespaceoid;
    AclResult aclresult;
    char* dictname = NULL;
    /* user-defined dictionary file list */
    List* userfiles = NIL;
    /* internal dictionary file name prefix */
    char* dictprefix = NULL;

    /* For now only user with sysadmin can create dictionary */
    if (!superuser())
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                (errmsg("must be system admin to CREATE TEXT SEARCH DICTIONARY"))));

    /* Convert list of names to a name and namespace */
    namespaceoid = QualifiedNameGetCreationNamespace(names, &dictname);
    if (isTempNamespace(namespaceoid))
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                (errmsg("CREATE TEXT SEARCH DICTIONARY in a temp namespace is not supported"))));

    /* Check we have creation rights in target namespace */
    aclresult = pg_namespace_aclcheck(namespaceoid, GetUserId(), ACL_CREATE);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error(aclresult, ACL_KIND_NAMESPACE, get_namespace_name(namespaceoid));

    /*
     * loop over the definition list and extract the information we need.
     */
    foreach (pl, parameters) {
        DefElem* defel = (DefElem*)lfirst(pl);

        if (pg_strcasecmp(defel->defname, "template") == 0) {
            templId = get_ts_template_oid(defGetQualifiedName(defel), false);
        } else {
            /* Assume it's an option for the dictionary itself */
            dictoptions = lappend(dictoptions, copyObject(defel));
        }
    }

    /*
     * Validation
     */
    if (!OidIsValid(templId))
        ereport(ERROR, (errcode(ERRCODE_INVALID_OBJECT_DEFINITION), errmsg("text search template is required")));

    /* get internal dictionary file prefix */
    dictprefix = get_tsfile_prefix_internal();
    /* return user file list if exists */
    userfiles = verify_dictoptions(templId, dictoptions, 0, dictprefix);
    /* no user-defined dictionary file */
    if (dictprefix != NULL && userfiles == NIL)
        pfree_ext(dictprefix);

    /*
     * Looks good, insert
     */
    errno_t rc = memset_s(values, sizeof(values), 0, sizeof(values));
    securec_check(rc, "", "");
    rc = memset_s(nulls, sizeof(nulls), false, sizeof(nulls));
    securec_check(rc, "", "");

    (void)namestrcpy(&dname, dictname);
    values[Anum_pg_ts_dict_dictname - 1] = NameGetDatum(&dname);
    values[Anum_pg_ts_dict_dictnamespace - 1] = ObjectIdGetDatum(namespaceoid);
    values[Anum_pg_ts_dict_dictowner - 1] = ObjectIdGetDatum(GetUserId());
    values[Anum_pg_ts_dict_dicttemplate - 1] = ObjectIdGetDatum(templId);
    if (dictoptions != NULL)
        values[Anum_pg_ts_dict_dictinitoption - 1] = PointerGetDatum(serialize_deflist(dictoptions));
    else
        nulls[Anum_pg_ts_dict_dictinitoption - 1] = true;

    dictRel = heap_open(TSDictionaryRelationId, RowExclusiveLock);
    tup = heap_form_tuple(dictRel->rd_att, values, nulls);
    dictOid = simple_heap_insert(dictRel, tup);

    CatalogUpdateIndexes(dictRel, tup);
    makeDictionaryDependencies(tup, dictprefix);
    /* Post creation hook for new text search dictionary */
    InvokeObjectAccessHook(OAT_POST_CREATE, TSDictionaryRelationId, dictOid, 0, NULL);
    tableam_tops_free_tuple(tup);
    heap_close(dictRel, RowExclusiveLock);

    /* Copy dictionary files to local/remote */
    if (dictprefix != NULL) {
        copy_tsfiles(userfiles, dictprefix);
        pfree_ext(dictprefix);
    }

    /* Clean up */
    if (userfiles != NIL)
        list_free_deep(userfiles);
}

/*
 * ALTER TEXT SEARCH DICTIONARY RENAME
 */
void RenameTSDictionary(List* oldname, const char* newname)
{
    HeapTuple tup;
    Relation rel;
    Oid dictId;
    Oid namespaceOid;
    AclResult aclresult;

    rel = heap_open(TSDictionaryRelationId, RowExclusiveLock);

    dictId = get_ts_dict_oid(oldname, false);
    /* Not allowed for built-in text search dictionary */
    if (!IsInitdb && dictId < FirstNormalObjectId)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Not allowed to alter built-in text search dictionary")));

    tup = SearchSysCacheCopy1(TSDICTOID, ObjectIdGetDatum(dictId));

    if (!HeapTupleIsValid(tup)) /* should not happen */
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for text search dictionary %u", dictId)));

    namespaceOid = ((Form_pg_ts_dict)GETSTRUCT(tup))->dictnamespace;

    if (SearchSysCacheExists2(TSDICTNAMENSP, PointerGetDatum(newname), ObjectIdGetDatum(namespaceOid)))
        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("text search dictionary \"%s\" already exists", newname)));

    /* must be owner */
    if (!pg_ts_dict_ownercheck(dictId, GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSDICTIONARY, NameListToString(oldname));

    /* must have CREATE privilege on namespace */
    aclresult = pg_namespace_aclcheck(namespaceOid, GetUserId(), ACL_CREATE);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error(aclresult, ACL_KIND_NAMESPACE, get_namespace_name(namespaceOid));

    (void)namestrcpy(&(((Form_pg_ts_dict)GETSTRUCT(tup))->dictname), newname);
    simple_heap_update(rel, &tup->t_self, tup);
    CatalogUpdateIndexes(rel, tup);

    heap_close(rel, NoLock);
    tableam_tops_free_tuple(tup);
}

/*
 * ALTER TEXT SEARCH DICTIONARY any_name SET SCHEMA name
 */
void AlterTSDictionaryNamespace(List* name, const char* newschema)
{
    Oid dictId, nspOid;
    Relation rel;

    rel = heap_open(TSDictionaryRelationId, RowExclusiveLock);

    dictId = get_ts_dict_oid(name, false);
    /* Not allowed for built-in text search dictionary */
    if (!IsInitdb && dictId < FirstNormalObjectId)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Not allowed to alter built-in text search dictionary")));

    /* get schema OID */
    nspOid = LookupCreationNamespace(newschema);

    (void)AlterObjectNamespace(rel,
        TSDICTOID,
        TSDICTNAMENSP,
        dictId,
        nspOid,
        Anum_pg_ts_dict_dictname,
        Anum_pg_ts_dict_dictnamespace,
        Anum_pg_ts_dict_dictowner,
        ACL_KIND_TSDICTIONARY);

    heap_close(rel, RowExclusiveLock);
}

Oid AlterTSDictionaryNamespace_oid(Oid dictId, Oid newNspOid)
{
    /* Not allowed for built-in text search dictionary */
    if (!IsInitdb && dictId < FirstNormalObjectId)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Not allowed to alter built-in text search dictionary")));

    Oid oldNspOid;
    Relation rel;

    rel = heap_open(TSDictionaryRelationId, RowExclusiveLock);

    oldNspOid = AlterObjectNamespace(rel,
        TSDICTOID,
        TSDICTNAMENSP,
        dictId,
        newNspOid,
        Anum_pg_ts_dict_dictname,
        Anum_pg_ts_dict_dictnamespace,
        Anum_pg_ts_dict_dictowner,
        ACL_KIND_TSDICTIONARY);

    heap_close(rel, RowExclusiveLock);

    return oldNspOid;
}

/*
 * Guts of TS dictionary deletion.
 */
void RemoveTSDictionaryById(Oid dictId)
{
    /* Not allowed for built-in text search dictionary */
    if (!IsInitdb && dictId < FirstNormalObjectId)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Not allowed to drop built-in text search dictionary")));

    Relation relation;
    HeapTuple tup;

    relation = heap_open(TSDictionaryRelationId, RowExclusiveLock);
    tup = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dictId));
    if (!HeapTupleIsValid(tup))
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for text search dictionary %u", dictId)));

    simple_heap_delete(relation, &tup->t_self);
    ReleaseSysCache(tup);
    heap_close(relation, RowExclusiveLock);
    /* Get dictionary files to be deleted if it is defined by user */
    deleteDictionaryTSFile(dictId);
}

/*
 * ALTER TEXT SEARCH DICTIONARY
 */
void AlterTSDictionary(AlterTSDictionaryStmt* stmt)
{
    HeapTuple tup, newtup;
    Relation rel;
    Oid dictId;
    ListCell* pl = NULL;
    List* dictoptions = NIL;
    Datum opt;
    bool isnull = false;
    Datum repl_val[Natts_pg_ts_dict];
    bool repl_null[Natts_pg_ts_dict];
    bool repl_repl[Natts_pg_ts_dict];

    /* record first N option old */
    int oldnum = 0;
    /* dictionary files to be updated */
    List* userfiles = NIL;
    /* new dictprefix if needed */
    char* newprefix = NULL;

    dictId = get_ts_dict_oid(stmt->dictname, false);
    /* Not allowed for built-in text search dictionary */
    if (!IsInitdb && dictId < FirstNormalObjectId)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Not allowed to alter built-in text search dictionary")));

    rel = heap_open(TSDictionaryRelationId, RowExclusiveLock);
    tup = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dictId));
    if (!HeapTupleIsValid(tup))
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for text search dictionary %u", dictId)));
    /* must be owner */
    if (!pg_ts_dict_ownercheck(dictId, GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSDICTIONARY, NameListToString(stmt->dictname));

    Oid tmplId = ((Form_pg_ts_dict)GETSTRUCT(tup))->dicttemplate;
    Oid dictowner = ((Form_pg_ts_dict)GETSTRUCT(tup))->dictowner;
    /* deserialize the existing set of options */
    opt = SysCacheGetAttr(TSDICTOID, tup, Anum_pg_ts_dict_dictinitoption, &isnull);
    if (!isnull)
        dictoptions = deserialize_deflist(opt);
    ereport(LOG,
        (errmodule(MOD_TS),
            errmsg("Update text search dictionary options (old): %s",
                isnull ? "NULL" : text_to_cstring(DatumGetTextP(opt)))));
    /*
     * Modify the options list as per specified changes
     */
    List* newoptions = NIL;
    foreach (pl, stmt->options) {
        DefElem* defel = (DefElem*)lfirst(pl);
        ListCell* cell = NULL;
        ListCell* prev = NULL;
        ListCell* next = NULL;

        /* Not allowed template parameter */
        if (pg_strcasecmp(defel->defname, "template") == 0)
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("Not allowed Template parameter in alter text search dictionary")));

        /*
         * Remove any matches ...
         */
        prev = NULL;
        for (cell = list_head(dictoptions); cell != NULL; cell = next) {
            DefElem* oldel = (DefElem*)lfirst(cell);

            next = lnext(cell);
            if (pg_strcasecmp(oldel->defname, defel->defname) == 0)
                dictoptions = list_delete_cell(dictoptions, cell, prev);
            else
                prev = cell;
        }

        /* Not add if no value */
        if (defel->arg != NULL)
            newoptions = lappend(newoptions, copyObject(defel));
    }

    /* construct options as first old then new */
    oldnum = list_length(dictoptions);
    dictoptions = list_concat(dictoptions, newoptions);

    /*
     * Validate
     */
    newprefix = get_tsfile_prefix_internal();
    userfiles = verify_dictoptions(tmplId, dictoptions, oldnum, newprefix);
    /* no user-defined dictionary file */
    if (newprefix != NULL && userfiles == NIL)
        pfree_ext(newprefix);

    /*
     * Looks good, update
     */
    errno_t rc = memset_s(repl_val, sizeof(repl_val), 0, sizeof(repl_val));
    securec_check(rc, "", "");
    rc = memset_s(repl_null, sizeof(repl_null), false, sizeof(repl_null));
    securec_check(rc, "", "");
    rc = memset_s(repl_repl, sizeof(repl_repl), false, sizeof(repl_repl));
    securec_check(rc, "", "");

    if (dictoptions != NULL) {
        repl_val[Anum_pg_ts_dict_dictinitoption - 1] = PointerGetDatum(serialize_deflist(dictoptions));
    } else {
        repl_null[Anum_pg_ts_dict_dictinitoption - 1] = true;
    }
    repl_repl[Anum_pg_ts_dict_dictinitoption - 1] = true;
    newtup = (HeapTuple) tableam_tops_modify_tuple(tup, RelationGetDescr(rel), repl_val, repl_null, repl_repl);

    simple_heap_update(rel, &newtup->t_self, newtup);
    CatalogUpdateIndexes(rel, newtup);
    /* Get old dictionary files to be deleted according to pg_shdepend */
    deleteDictionaryTSFile(dictId);

    /* Update pg_shdepend if needed */
    if (newprefix != NULL) {
        StringInfoData objfile;
        initStringInfo(&objfile);
        appendStringInfo(&objfile, "%s%c%u", newprefix, DICT_SEPARATOR, tmplId);
        changeDependencyOnObjfile(dictId, dictowner, objfile.data);
        /* Clean up */
        pfree_ext(objfile.data);
    } else
        changeDependencyOnObjfile(dictId, dictowner, NULL);

    /*
     * NOTE: because we only support altering the options, not the template,
     * there is no need to update dependencies.  This might have to change if
     * the options ever reference inside-the-database objects.
     */
    tableam_tops_free_tuple(newtup);
    ReleaseSysCache(tup);
    heap_close(rel, RowExclusiveLock);

    /* copy dictionary files to local and remote if needed */
    if (newprefix != NULL) {
        copy_tsfiles(userfiles, newprefix);
        pfree_ext(newprefix);
    }

    /* Clean up */
    if (userfiles != NIL)
        list_free_deep(userfiles);
}

/*
 * @Description: internal process for ALTER TEXT SEARCH DICTIONARY OWNER.
 * @in rel: pg_ts_dict relation.
 * @in dictId: dict id.
 * @in newOwnerId: new owner id.
 * @out: void
 */
void AlterTSDictionaryOwner_internal(Relation rel, Oid dictId, Oid newOwnerId)
{
    HeapTuple tup;
    Oid namespaceOid;
    AclResult aclresult;
    Form_pg_ts_dict form;

    tup = SearchSysCacheCopy1(TSDICTOID, ObjectIdGetDatum(dictId));

    if (!HeapTupleIsValid(tup)) /* should not happen */
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for text search dictionary %u", dictId)));

    form = (Form_pg_ts_dict)GETSTRUCT(tup);
    namespaceOid = form->dictnamespace;

    if (form->dictowner != newOwnerId) {
        /* Superusers can always do it */
        if (!superuser()) {
            /* must be owner */
            if (!pg_ts_dict_ownercheck(dictId, GetUserId()))
                aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSDICTIONARY, NameStr(form->dictname));

            /* Must be able to become new owner */
            check_is_member_of_role(GetUserId(), newOwnerId);

            /* New owner must have CREATE privilege on namespace */
            aclresult = pg_namespace_aclcheck(namespaceOid, newOwnerId, ACL_CREATE);
            if (aclresult != ACLCHECK_OK)
                aclcheck_error(aclresult, ACL_KIND_NAMESPACE, get_namespace_name(namespaceOid));
        }

        form->dictowner = newOwnerId;

        simple_heap_update(rel, &tup->t_self, tup);
        CatalogUpdateIndexes(rel, tup);

        /* Update owner dependency reference */
        changeDependencyOnOwner(TSDictionaryRelationId, HeapTupleGetOid(tup), newOwnerId);
    }

    tableam_tops_free_tuple(tup);
}

/*
 * @Description: ALTER TEXT SEARCH DICTIONARY OWNER by dict id.
 * @in dictId: dict id.
 * @in newOwnerId: new owner id.
 * @out: void
 */
void AlterTSDictionaryOwner_oid(Oid dictId, Oid newOwnerId)
{
    /* Not allowed for built-in text search dictionary */
    if (!IsInitdb && dictId < FirstNormalObjectId)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Not allowed to alter built-in text search dictionary")));

    Relation rel = heap_open(TSDictionaryRelationId, RowExclusiveLock);
    AlterTSDictionaryOwner_internal(rel, dictId, newOwnerId);
    heap_close(rel, NoLock);
}

/*
 * ALTER TEXT SEARCH DICTIONARY OWNER
 */
void AlterTSDictionaryOwner(List* name, Oid newOwnerId)
{
    Relation rel;
    Oid dictId;

    rel = heap_open(TSDictionaryRelationId, RowExclusiveLock);
    dictId = get_ts_dict_oid(name, false);
    /* Not allowed for built-in text search dictionary */
    if (!IsInitdb && dictId < FirstNormalObjectId)
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Not allowed to alter built-in text search dictionary")));

    AlterTSDictionaryOwner_internal(rel, dictId, newOwnerId);
    heap_close(rel, NoLock);
}

/* ---------------------- TS Template commands ----------------------- */
/*
 * lookup a template support function and return its OID (as a Datum)
 *
 * attnum is the pg_ts_template column the function will go into
 */
static Datum get_ts_template_func(DefElem* defel, int attnum)
{
    List* funcName = defGetQualifiedName(defel);
    Oid typeId[4];
    Oid retTypeId;
    int nargs;
    Oid procOid;

    retTypeId = INTERNALOID;
    typeId[0] = INTERNALOID;
    typeId[1] = INTERNALOID;
    typeId[2] = INTERNALOID;
    typeId[3] = INTERNALOID;
    switch (attnum) {
        case Anum_pg_ts_template_tmplinit:
            nargs = 1;
            break;
        case Anum_pg_ts_template_tmpllexize:
            nargs = 4;
            break;
        default:
            /* should not be here */
            ereport(ERROR,
                (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                    errmsg("unrecognized attribute for text search template: %d", attnum)));
            nargs = 0; /* keep compiler quiet */
    }

    procOid = LookupFuncName(funcName, nargs, typeId, false);
    if (get_func_rettype(procOid) != retTypeId)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                errmsg("function %s should return type %s",
                    func_signature_string(funcName, nargs, NIL, typeId),
                    format_type_be(retTypeId))));

    return ObjectIdGetDatum(procOid);
}

/*
 * make pg_depend entries for a new pg_ts_template entry
 */
static void makeTSTemplateDependencies(HeapTuple tuple)
{
    Form_pg_ts_template tmpl = (Form_pg_ts_template)GETSTRUCT(tuple);
    ObjectAddress myself, referenced;

    myself.classId = TSTemplateRelationId;
    myself.objectId = HeapTupleGetOid(tuple);
    myself.objectSubId = 0;

    /* dependency on namespace */
    referenced.classId = NamespaceRelationId;
    referenced.objectId = tmpl->tmplnamespace;
    referenced.objectSubId = 0;
    recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

    /* dependency on extension */
    recordDependencyOnCurrentExtension(&myself, false);

    /* dependencies on functions */
    referenced.classId = ProcedureRelationId;
    referenced.objectSubId = 0;
    referenced.objectId = tmpl->tmpllexize;
    recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

    if (OidIsValid(tmpl->tmplinit)) {
        referenced.objectId = tmpl->tmplinit;
        recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
    }
}

/*
 * CREATE TEXT SEARCH TEMPLATE
 */
void DefineTSTemplate(List* names, List* parameters)
{
    ListCell* pl = NULL;
    Relation tmplRel;
    HeapTuple tup;
    Datum values[Natts_pg_ts_template];
    bool nulls[Natts_pg_ts_template];
    NameData dname;
    int i;
    Oid dictOid;
    Oid namespaceoid;
    char* tmplname = NULL;

    if (!superuser())
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be system admin to create text search templates")));

    /* Convert list of names to a name and namespace */
    namespaceoid = QualifiedNameGetCreationNamespace(names, &tmplname);

    for (i = 0; i < Natts_pg_ts_template; i++) {
        nulls[i] = false;
        values[i] = ObjectIdGetDatum(InvalidOid);
    }

    (void)namestrcpy(&dname, tmplname);
    values[Anum_pg_ts_template_tmplname - 1] = NameGetDatum(&dname);
    values[Anum_pg_ts_template_tmplnamespace - 1] = ObjectIdGetDatum(namespaceoid);

    /*
     * loop over the definition list and extract the information we need.
     */
    foreach (pl, parameters) {
        DefElem* defel = (DefElem*)lfirst(pl);

        if (pg_strcasecmp(defel->defname, "init") == 0) {
            values[Anum_pg_ts_template_tmplinit - 1] = get_ts_template_func(defel, Anum_pg_ts_template_tmplinit);
            nulls[Anum_pg_ts_template_tmplinit - 1] = false;
        } else if (pg_strcasecmp(defel->defname, "lexize") == 0) {
            values[Anum_pg_ts_template_tmpllexize - 1] = get_ts_template_func(defel, Anum_pg_ts_template_tmpllexize);
            nulls[Anum_pg_ts_template_tmpllexize - 1] = false;
        } else
            ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg("text search template parameter \"%s\" not recognized", defel->defname)));
    }

    /*
     * Validation
     */
    if (!OidIsValid(DatumGetObjectId(values[Anum_pg_ts_template_tmpllexize - 1])))
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_OBJECT_DEFINITION), errmsg("text search template lexize method is required")));

    /*
     * Looks good, insert
     */
    tmplRel = heap_open(TSTemplateRelationId, RowExclusiveLock);
    tup = heap_form_tuple(tmplRel->rd_att, values, nulls);
    dictOid = simple_heap_insert(tmplRel, tup);

    CatalogUpdateIndexes(tmplRel, tup);
    makeTSTemplateDependencies(tup);
    /* Post creation hook for new text search template */
    InvokeObjectAccessHook(OAT_POST_CREATE, TSTemplateRelationId, dictOid, 0, NULL);
    tableam_tops_free_tuple(tup);
    heap_close(tmplRel, RowExclusiveLock);
}

/*
 * ALTER TEXT SEARCH TEMPLATE RENAME
 */
void RenameTSTemplate(List* oldname, const char* newname)
{
    HeapTuple tup;
    Relation rel;
    Oid tmplId;
    Oid namespaceOid;

    if (!superuser())
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be system admin to rename text search templates")));

    rel = heap_open(TSTemplateRelationId, RowExclusiveLock);
    tmplId = get_ts_template_oid(oldname, false);
    tup = SearchSysCacheCopy1(TSTEMPLATEOID, ObjectIdGetDatum(tmplId));
    if (!HeapTupleIsValid(tup)) /* should not happen */
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for text search template %u", tmplId)));

    namespaceOid = ((Form_pg_ts_template)GETSTRUCT(tup))->tmplnamespace;
    if (SearchSysCacheExists2(TSTEMPLATENAMENSP, PointerGetDatum(newname), ObjectIdGetDatum(namespaceOid)))
        ereport(
            ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("text search template \"%s\" already exists", newname)));

    (void)namestrcpy(&(((Form_pg_ts_template)GETSTRUCT(tup))->tmplname), newname);
    simple_heap_update(rel, &tup->t_self, tup);
    CatalogUpdateIndexes(rel, tup);

    heap_close(rel, NoLock);
    tableam_tops_free_tuple(tup);
}

/*
 * ALTER TEXT SEARCH TEMPLATE any_name SET SCHEMA name
 */
void AlterTSTemplateNamespace(List* name, const char* newschema)
{
    Oid tmplId, nspOid;
    Relation rel;

    rel = heap_open(TSTemplateRelationId, RowExclusiveLock);
    tmplId = get_ts_template_oid(name, false);
    /* get schema OID */
    nspOid = LookupCreationNamespace(newschema);

    (void)AlterObjectNamespace(rel,
        TSTEMPLATEOID,
        TSTEMPLATENAMENSP,
        tmplId,
        nspOid,
        Anum_pg_ts_template_tmplname,
        Anum_pg_ts_template_tmplnamespace,
        -1,
        (AclObjectKind)-1);

    heap_close(rel, RowExclusiveLock);
}

Oid AlterTSTemplateNamespace_oid(Oid tmplId, Oid newNspOid)
{
    Oid oldNspOid;
    Relation rel;

    rel = heap_open(TSTemplateRelationId, RowExclusiveLock);

    oldNspOid = AlterObjectNamespace(rel,
        TSTEMPLATEOID,
        TSTEMPLATENAMENSP,
        tmplId,
        newNspOid,
        Anum_pg_ts_template_tmplname,
        Anum_pg_ts_template_tmplnamespace,
        -1,
        (AclObjectKind)-1);

    heap_close(rel, RowExclusiveLock);
    return oldNspOid;
}

/*
 * Guts of TS template deletion.
 */
void RemoveTSTemplateById(Oid tmplId)
{
    Relation relation;
    HeapTuple tup;

    relation = heap_open(TSTemplateRelationId, RowExclusiveLock);
    tup = SearchSysCache1(TSTEMPLATEOID, ObjectIdGetDatum(tmplId));
    if (!HeapTupleIsValid(tup))
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for text search template %u", tmplId)));

    simple_heap_delete(relation, &tup->t_self);
    ReleaseSysCache(tup);
    heap_close(relation, RowExclusiveLock);
}

/* ---------------------- TS Configuration commands ----------------------- */
/*
 * Finds syscache tuple of configuration.
 * Returns NULL if no such cfg.
 */
static HeapTuple GetTSConfigTuple(List* names)
{
    HeapTuple tup;
    Oid cfgId;

    cfgId = get_ts_config_oid(names, true);
    if (!OidIsValid(cfgId))
        return NULL;

    tup = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(cfgId));

    if (!HeapTupleIsValid(tup)) /* should not happen */
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for text search configuration %u", cfgId)));

    return tup;
}

/*
 * make pg_depend entries for a new or updated pg_ts_config entry
 *
 * Pass opened pg_ts_config_map relation if there might be any config map
 * entries for the config.
 */
static void makeConfigurationDependencies(HeapTuple tuple, bool removeOld, Relation mapRel)
{
    Form_pg_ts_config cfg = (Form_pg_ts_config)GETSTRUCT(tuple);
    ObjectAddresses* addrs = NULL;
    ObjectAddress myself, referenced;

    myself.classId = TSConfigRelationId;
    myself.objectId = HeapTupleGetOid(tuple);
    myself.objectSubId = 0;

    /* for ALTER case, first flush old dependencies, except extension deps */
    if (removeOld) {
        (void)deleteDependencyRecordsFor(myself.classId, myself.objectId, true);
        deleteSharedDependencyRecordsFor(myself.classId, myself.objectId, 0);
    }

    /*
     * We use an ObjectAddresses list to remove possible duplicate
     * dependencies from the config map info.  The pg_ts_config items
     * shouldn't be duplicates, but might as well fold them all into one call.
     */
    addrs = new_object_addresses();

    /* dependency on namespace */
    referenced.classId = NamespaceRelationId;
    referenced.objectId = cfg->cfgnamespace;
    referenced.objectSubId = 0;
    add_exact_object_address(&referenced, addrs);
    /* dependency on owner */
    recordDependencyOnOwner(myself.classId, myself.objectId, cfg->cfgowner);
    /* dependency on extension */
    recordDependencyOnCurrentExtension(&myself, removeOld);
    /* dependency on parser */
    referenced.classId = TSParserRelationId;
    referenced.objectId = cfg->cfgparser;
    referenced.objectSubId = 0;
    add_exact_object_address(&referenced, addrs);
    /* dependencies on dictionaries listed in config map */
    if (mapRel) {
        ScanKeyData skey;
        SysScanDesc scan;
        HeapTuple maptup;
        /* CCI to ensure we can see effects of caller's changes */
        CommandCounterIncrement();

        ScanKeyInit(
            &skey, Anum_pg_ts_config_map_mapcfg, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(myself.objectId));

        scan = systable_beginscan(mapRel, TSConfigMapIndexId, true, SnapshotNow, 1, &skey);
        while (HeapTupleIsValid((maptup = systable_getnext(scan)))) {
            Form_pg_ts_config_map cfgmap = (Form_pg_ts_config_map)GETSTRUCT(maptup);

            referenced.classId = TSDictionaryRelationId;
            referenced.objectId = cfgmap->mapdict;
            referenced.objectSubId = 0;
            add_exact_object_address(&referenced, addrs);
        }
        systable_endscan(scan);
    }

    /* Record 'em (this includes duplicate elimination) */
    record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
    free_object_addresses(addrs);
}

/*
 * CREATE TEXT SEARCH CONFIGURATION
 */
void DefineTSConfiguration(List* names, List* parameters, List* cfoptions)
{
    Relation cfgRel;
    Relation mapRel = NULL;
    HeapTuple tup;
    Datum values[Natts_pg_ts_config];
    bool nulls[Natts_pg_ts_config];
    AclResult aclresult;
    Oid namespaceoid;
    char* cfgname = NULL;
    NameData cname;
    Oid sourceOid = InvalidOid;
    Oid prsOid = InvalidOid;
    Oid cfgOid;
    ListCell* pl = NULL;
    Datum newOptions;

    /* Convert list of names to a name and namespace */
    namespaceoid = QualifiedNameGetCreationNamespace(names, &cfgname);

    /* Check we have creation rights in target namespace */
    aclresult = pg_namespace_aclcheck(namespaceoid, GetUserId(), ACL_CREATE);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error(aclresult, ACL_KIND_NAMESPACE, get_namespace_name(namespaceoid));

    /*
     * loop over the definition list and extract the information we need.
     */
    foreach (pl, parameters) {
        DefElem* defel = (DefElem*)lfirst(pl);

        if (pg_strcasecmp(defel->defname, "parser") == 0)
            prsOid = get_ts_parser_oid(defGetQualifiedName(defel), false);
        else if (pg_strcasecmp(defel->defname, "copy") == 0)
            sourceOid = get_ts_config_oid(defGetQualifiedName(defel), false);
        else
            ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg("text search configuration parameter \"%s\" not recognized", defel->defname)));
    }

    if (OidIsValid(sourceOid) && OidIsValid(prsOid))
        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("cannot specify both PARSER and COPY options")));

    /*
     * Look up source config if given.
     */
    if (OidIsValid(sourceOid)) {
        Form_pg_ts_config cfg;

        tup = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(sourceOid));
        if (!HeapTupleIsValid(tup))
            ereport(ERROR,
                (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                    errmsg("cache lookup failed for text search configuration %u", sourceOid)));

        cfg = (Form_pg_ts_config)GETSTRUCT(tup);

        /* use source's parser */
        prsOid = cfg->cfgparser;

        ReleaseSysCache(tup);
    }

    /*
     * Validation
     */
    if (!OidIsValid(prsOid))
        ereport(ERROR, (errcode(ERRCODE_INVALID_OBJECT_DEFINITION), errmsg("text search parser is required")));
    if (prsOid == ZHPARSER_PARSER) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_OBJECT_DEFINITION), errmsg("Zhparser is not supported!")));
    }

    /*
     * Looks good, build tuple and insert
     */
    errno_t rc = memset_s(values, sizeof(values), 0, sizeof(values));
    securec_check(rc, "", "");
    rc = memset_s(nulls, sizeof(nulls), false, sizeof(nulls));
    securec_check(rc, "", "");
    (void)namestrcpy(&cname, cfgname);
    values[Anum_pg_ts_config_cfgname - 1] = NameGetDatum(&cname);
    values[Anum_pg_ts_config_cfgnamespace - 1] = ObjectIdGetDatum(namespaceoid);
    values[Anum_pg_ts_config_cfgowner - 1] = ObjectIdGetDatum(GetUserId());
    values[Anum_pg_ts_config_cfgparser - 1] = ObjectIdGetDatum(prsOid);

    /* parse configuration options */
    if (cfoptions != NULL) {
        Datum oldOptions;
        bool isnull = true;
        HeapTuple tuple;

        if (OidIsValid(sourceOid)) {
            tuple = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(sourceOid));
            if (!HeapTupleIsValid(tuple))
                ereport(ERROR,
                    (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                        errmsg("cache lookup failed for text search configuration %u", sourceOid)));
            oldOptions = SysCacheGetAttr(TSCONFIGOID, tuple, Anum_pg_ts_config_cfoptions, &isnull);
            ReleaseSysCache(tuple);
        }
        /* transform text search configuration options into the text array */
        newOptions = transformRelOptions(isnull ? (Datum)0 : oldOptions, cfoptions, NULL, NULL, false, false);
        /*  check the options */
        (void)tsearch_config_reloptions(newOptions, true, prsOid, false);
        values[Anum_pg_ts_config_cfoptions - 1] = newOptions;
    } else {
        nulls[Anum_pg_ts_config_cfoptions - 1] = true;
    }

    cfgRel = heap_open(TSConfigRelationId, RowExclusiveLock);
    tup = heap_form_tuple(cfgRel->rd_att, values, nulls);
    cfgOid = simple_heap_insert(cfgRel, tup);
    CatalogUpdateIndexes(cfgRel, tup);

    if (OidIsValid(sourceOid)) {
        /*
         * Copy token-dicts map from source config
         */
        ScanKeyData skey;
        SysScanDesc scan;
        HeapTuple maptup;

        mapRel = heap_open(TSConfigMapRelationId, RowExclusiveLock);
        ScanKeyInit(&skey, Anum_pg_ts_config_map_mapcfg, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(sourceOid));
        scan = systable_beginscan(mapRel, TSConfigMapIndexId, true, SnapshotNow, 1, &skey);
        while (HeapTupleIsValid((maptup = systable_getnext(scan)))) {
            Form_pg_ts_config_map cfgmap = (Form_pg_ts_config_map)GETSTRUCT(maptup);
            HeapTuple newmaptup;
            Datum mapvalues[Natts_pg_ts_config_map];
            bool mapnulls[Natts_pg_ts_config_map];

            rc = EOK;
            rc = memset_s(mapvalues, sizeof(mapvalues), 0, sizeof(mapvalues));
            securec_check(rc, "", "");
            rc = memset_s(mapnulls, sizeof(mapnulls), false, sizeof(mapnulls));
            securec_check(rc, "", "");

            mapvalues[Anum_pg_ts_config_map_mapcfg - 1] = cfgOid;
            mapvalues[Anum_pg_ts_config_map_maptokentype - 1] = cfgmap->maptokentype;
            mapvalues[Anum_pg_ts_config_map_mapseqno - 1] = cfgmap->mapseqno;
            mapvalues[Anum_pg_ts_config_map_mapdict - 1] = cfgmap->mapdict;

            newmaptup = heap_form_tuple(mapRel->rd_att, mapvalues, mapnulls);

            (void)simple_heap_insert(mapRel, newmaptup);
            CatalogUpdateIndexes(mapRel, newmaptup);
            tableam_tops_free_tuple(newmaptup);
        }

        systable_endscan(scan);
    }

    makeConfigurationDependencies(tup, false, mapRel);

    /* Post creation hook for new text search configuration */
    InvokeObjectAccessHook(OAT_POST_CREATE, TSConfigRelationId, cfgOid, 0, NULL);

    tableam_tops_free_tuple(tup);

    if (mapRel)
        heap_close(mapRel, RowExclusiveLock);
    heap_close(cfgRel, RowExclusiveLock);
}

/*
 * ALTER TEXT SEARCH CONFIGURATION RENAME
 */
void RenameTSConfiguration(List* oldname, const char* newname)
{
    HeapTuple tup;
    Relation rel;
    Oid cfgId;
    AclResult aclresult;
    Oid namespaceOid;

    rel = heap_open(TSConfigRelationId, RowExclusiveLock);
    cfgId = get_ts_config_oid(oldname, false);
    tup = SearchSysCacheCopy1(TSCONFIGOID, ObjectIdGetDatum(cfgId));

    if (!HeapTupleIsValid(tup)) /* should not happen */
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for text search configuration %u", cfgId)));

    namespaceOid = ((Form_pg_ts_config)GETSTRUCT(tup))->cfgnamespace;

    if (SearchSysCacheExists2(TSCONFIGNAMENSP, PointerGetDatum(newname), ObjectIdGetDatum(namespaceOid)))
        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("text search configuration \"%s\" already exists", newname)));

    /* must be owner */
    if (!pg_ts_config_ownercheck(cfgId, GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSCONFIGURATION, NameListToString(oldname));

    /* must have CREATE privilege on namespace */
    aclresult = pg_namespace_aclcheck(namespaceOid, GetUserId(), ACL_CREATE);
    aclcheck_error(aclresult, ACL_KIND_NAMESPACE, get_namespace_name(namespaceOid));

    (void)namestrcpy(&(((Form_pg_ts_config)GETSTRUCT(tup))->cfgname), newname);
    simple_heap_update(rel, &tup->t_self, tup);
    CatalogUpdateIndexes(rel, tup);

    heap_close(rel, NoLock);
    tableam_tops_free_tuple(tup);
}

/*
 * ALTER TEXT SEARCH CONFIGURATION any_name SET SCHEMA name
 */
void AlterTSConfigurationNamespace(List* name, const char* newschema)
{
    Oid cfgId, nspOid;
    Relation rel;

    rel = heap_open(TSConfigRelationId, RowExclusiveLock);
    cfgId = get_ts_config_oid(name, false);
    /* get schema OID */
    nspOid = LookupCreationNamespace(newschema);

    (void)AlterObjectNamespace(rel,
        TSCONFIGOID,
        TSCONFIGNAMENSP,
        cfgId,
        nspOid,
        Anum_pg_ts_config_cfgname,
        Anum_pg_ts_config_cfgnamespace,
        Anum_pg_ts_config_cfgowner,
        ACL_KIND_TSCONFIGURATION);

    heap_close(rel, RowExclusiveLock);
}

Oid AlterTSConfigurationNamespace_oid(Oid cfgId, Oid newNspOid)
{
    Oid oldNspOid;
    Relation rel;

    rel = heap_open(TSConfigRelationId, RowExclusiveLock);

    oldNspOid = AlterObjectNamespace(rel,
        TSCONFIGOID,
        TSCONFIGNAMENSP,
        cfgId,
        newNspOid,
        Anum_pg_ts_config_cfgname,
        Anum_pg_ts_config_cfgnamespace,
        Anum_pg_ts_config_cfgowner,
        ACL_KIND_TSCONFIGURATION);

    heap_close(rel, RowExclusiveLock);

    return oldNspOid;
}

/*
 * Guts of TS configuration deletion.
 */
void RemoveTSConfigurationById(Oid cfgId)
{
    Relation relCfg, relMap;
    HeapTuple tup;
    ScanKeyData skey;
    SysScanDesc scan;

    /* Remove the pg_ts_config entry */
    relCfg = heap_open(TSConfigRelationId, RowExclusiveLock);

    tup = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(cfgId));

    if (!HeapTupleIsValid(tup))
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for text search dictionary %u", cfgId)));

    simple_heap_delete(relCfg, &tup->t_self);
    ReleaseSysCache(tup);
    heap_close(relCfg, RowExclusiveLock);

    /* Remove any pg_ts_config_map entries */
    relMap = heap_open(TSConfigMapRelationId, RowExclusiveLock);
    ScanKeyInit(&skey, Anum_pg_ts_config_map_mapcfg, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(cfgId));
    scan = systable_beginscan(relMap, TSConfigMapIndexId, true, SnapshotNow, 1, &skey);

    while (HeapTupleIsValid((tup = systable_getnext(scan)))) {
        simple_heap_delete(relMap, &tup->t_self);
    }

    systable_endscan(scan);
    heap_close(relMap, RowExclusiveLock);
}

/*
 * ALTER TEXT SEARCH CONFIGURATION OWNER
 */
void AlterTSConfigurationOwner(List* name, Oid newOwnerId)
{
    HeapTuple tup;
    Relation rel;
    Oid cfgId;
    AclResult aclresult;
    Oid namespaceOid;
    Form_pg_ts_config form;

    rel = heap_open(TSConfigRelationId, RowExclusiveLock);
    cfgId = get_ts_config_oid(name, false);
    tup = SearchSysCacheCopy1(TSCONFIGOID, ObjectIdGetDatum(cfgId));
    if (!HeapTupleIsValid(tup)) /* should not happen */
        ereport(ERROR,
            (errcode(ERRCODE_CACHE_LOOKUP_FAILED),
                errmsg("cache lookup failed for text search configuration %u", cfgId)));
    form = (Form_pg_ts_config)GETSTRUCT(tup);
    namespaceOid = form->cfgnamespace;

    if (form->cfgowner != newOwnerId) {
        /* Superusers can always do it */
        if (!superuser()) {
            /* must be owner */
            if (!pg_ts_config_ownercheck(cfgId, GetUserId()))
                aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSCONFIGURATION, NameListToString(name));

            /* Must be able to become new owner */
            check_is_member_of_role(GetUserId(), newOwnerId);

            /* New owner must have CREATE privilege on namespace */
            aclresult = pg_namespace_aclcheck(namespaceOid, newOwnerId, ACL_CREATE);
            if (aclresult != ACLCHECK_OK)
                aclcheck_error(aclresult, ACL_KIND_NAMESPACE, get_namespace_name(namespaceOid));
        }

        form->cfgowner = newOwnerId;

        simple_heap_update(rel, &tup->t_self, tup);
        CatalogUpdateIndexes(rel, tup);

        /* Update owner dependency reference */
        changeDependencyOnOwner(TSConfigRelationId, HeapTupleGetOid(tup), newOwnerId);
    }

    heap_close(rel, NoLock);
    tableam_tops_free_tuple(tup);
}

/*
 * ALTER TEXT SEARCH CONFIGURATION - main entry point
 */
void AlterTSConfiguration(AlterTSConfigurationStmt* stmt)
{
    HeapTuple tup;
    Relation relMap;

    /* Find the configuration */
    tup = GetTSConfigTuple(stmt->cfgname);
    if (!HeapTupleIsValid(tup))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("text search configuration \"%s\" does not exist", NameListToString(stmt->cfgname))));

    if (TSConfigurationHasDependentObjects(HeapTupleGetOid(tup))) {
        ReleaseSysCache(tup);
        ereport(ERROR,
            (errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
                errmsg("cannot alter text search configuration \"%s\" because other objects depend on it",
                    NameListToString(stmt->cfgname))));
    }

    /* must be owner */
    if (!pg_ts_config_ownercheck(HeapTupleGetOid(tup), GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TSCONFIGURATION, NameListToString(stmt->cfgname));

    relMap = heap_open(TSConfigMapRelationId, RowExclusiveLock);

    /* Add or drop mappings or modify configuration para */
    if (stmt->dicts)
        MakeConfigurationMapping(stmt, tup, relMap);
    else if (stmt->tokentype)
        DropConfigurationMapping(stmt, tup, relMap);
    else if (stmt->cfoptions)
        SetConfigurationOptions(stmt, tup);

    /* Update dependencies */
    makeConfigurationDependencies(tup, true, relMap);

    heap_close(relMap, RowExclusiveLock);
    ReleaseSysCache(tup);
}

/*
 * Translate a list of token type names to an array of token type numbers
 */
static int* getTokenTypes(Oid prsId, List* tokenNames)
{
    TSParserCacheEntry* prs = lookup_ts_parser_cache(prsId);
    LexDescr* list = NULL;
    int* res = NULL;
    int i;
    int ntoken;
    ListCell* tn = NULL;

    ntoken = list_length(tokenNames);
    if (ntoken == 0)
        return NULL;
    res = (int*)palloc(sizeof(int) * ntoken);

    if (!OidIsValid(prs->lextypeOid))
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("method lextype isn't defined for text search parser %u", prsId)));

    /* lextype takes one dummy argument */
    list = (LexDescr*)DatumGetPointer(OidFunctionCall1(prs->lextypeOid, (Datum)0));

    i = 0;
    foreach (tn, tokenNames) {
        Value* val = (Value*)lfirst(tn);
        bool found = false;
        int j;

        j = 0;
        while (list != NULL && list[j].lexid) {
            /* XXX should we use pg_strcasecmp here? */
            if (strcmp(strVal(val), list[j].alias) == 0) {
                res[i] = list[j].lexid;
                found = true;
                break;
            }
            j++;
        }
        if (!found)
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("token type \"%s\" does not exist", strVal(val))));
        i++;
    }

    return res;
}

/*
 * @Description:  ALTER TEXT SEARCH CONFIGURATION SET
 *
 * @in stmt: alter text search configuration Statement,
 * @in tup: HeapTuple for the special configuration, which will be modified
 */
static void SetConfigurationOptions(AlterTSConfigurationStmt* stmt, HeapTuple tup)
{
    Relation relMap = heap_open(TSConfigRelationId, RowExclusiveLock);
    Datum newOptions; /* new configuration options */
    Datum oldOptions; /* old configuration options */
    Datum val[Natts_pg_ts_config];
    bool isnull = false;
    bool null[Natts_pg_ts_config];
    bool repl[Natts_pg_ts_config];
    Oid prsId = ((Form_pg_ts_config)GETSTRUCT(tup))->cfgparser; /* corresponding parser */
    HeapTuple newtuple;

    oldOptions = SysCacheGetAttr(TSCONFIGOID, tup, Anum_pg_ts_config_cfoptions, &isnull);

    /* transform text search configuration */
    newOptions =
        transformRelOptions(isnull ? (Datum)0 : oldOptions, stmt->cfoptions, NULL, NULL, false, stmt->is_reset);
    /* check text search configuration */
    (void)tsearch_config_reloptions(newOptions, true, prsId, false);

    errno_t rc = memset_s(val, sizeof(val), 0, sizeof(val));
    securec_check(rc, "", "");
    rc = memset_s(null, sizeof(null), false, sizeof(null));
    securec_check(rc, "", "");
    rc = memset_s(repl, sizeof(repl), false, sizeof(repl));
    securec_check(rc, "", "");

    if (newOptions == (Datum)0)
        null[Anum_pg_ts_config_cfoptions - 1] = true;
    else
        val[Anum_pg_ts_config_cfoptions - 1] = newOptions;
    repl[Anum_pg_ts_config_cfoptions - 1] = true;

    newtuple = (HeapTuple) tableam_tops_modify_tuple(tup, RelationGetDescr(relMap), val, null, repl);
    simple_heap_update(relMap, &newtuple->t_self, newtuple);
    CatalogUpdateIndexes(relMap, newtuple);

    tableam_tops_free_tuple(newtuple);
    heap_close(relMap, NoLock);
}

/*
 * ALTER TEXT SEARCH CONFIGURATION ADD/ALTER MAPPING
 */
static void MakeConfigurationMapping(AlterTSConfigurationStmt* stmt, HeapTuple tup, Relation relMap)
{
    Oid cfgId = HeapTupleGetOid(tup);
    ScanKeyData skey[2];
    SysScanDesc scan;
    HeapTuple maptup;
    int i;
    int j;
    Oid prsId;
    int* tokens = NULL;
    int ntoken;
    Oid* dictIds = NULL;
    int ndict;
    ListCell* c = NULL;

    prsId = ((Form_pg_ts_config)GETSTRUCT(tup))->cfgparser;

    tokens = getTokenTypes(prsId, stmt->tokentype);
    ntoken = list_length(stmt->tokentype);

    if (stmt->override) {
        /*
         * delete maps for tokens if they exist and command was ALTER
         */
        for (i = 0; i < ntoken; i++) {
            ScanKeyInit(
                &skey[0], Anum_pg_ts_config_map_mapcfg, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(cfgId));
            ScanKeyInit(&skey[1],
                Anum_pg_ts_config_map_maptokentype,
                BTEqualStrategyNumber,
                F_INT4EQ,
                Int32GetDatum(tokens[i]));

            scan = systable_beginscan(relMap, TSConfigMapIndexId, true, SnapshotNow, 2, skey);

            while (HeapTupleIsValid((maptup = systable_getnext(scan)))) {
                simple_heap_delete(relMap, &maptup->t_self);
            }

            systable_endscan(scan);
        }
    }

    /*
     * Convert list of dictionary names to array of dict OIDs
     */
    ndict = list_length(stmt->dicts);
    dictIds = (Oid*)palloc(sizeof(Oid) * ndict);
    i = 0;
    foreach (c, stmt->dicts) {
        List* names = (List*)lfirst(c);

        dictIds[i] = get_ts_dict_oid(names, false);
        i++;
    }

    if (stmt->replace) {
        /*
         * Replace a specific dictionary in existing entries
         */
        Oid dictOld = dictIds[0];
        Oid dictNew = dictIds[1];

        ScanKeyInit(&skey[0], Anum_pg_ts_config_map_mapcfg, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(cfgId));

        scan = systable_beginscan(relMap, TSConfigMapIndexId, true, SnapshotNow, 1, skey);

        while (HeapTupleIsValid((maptup = systable_getnext(scan)))) {
            Form_pg_ts_config_map cfgmap = (Form_pg_ts_config_map)GETSTRUCT(maptup);

            /*
             * check if it's one of target token types
             */
            if (tokens != NULL) {
                bool tokmatch = false;

                for (j = 0; j < ntoken; j++) {
                    if (cfgmap->maptokentype == tokens[j]) {
                        tokmatch = true;
                        break;
                    }
                }
                if (!tokmatch)
                    continue;
            }

            /*
             * replace dictionary if match
             */
            if (cfgmap->mapdict == dictOld) {
                Datum repl_val[Natts_pg_ts_config_map];
                bool repl_null[Natts_pg_ts_config_map];
                bool repl_repl[Natts_pg_ts_config_map];
                HeapTuple newtup;
                errno_t rc = EOK;
                rc = memset_s(repl_val, sizeof(repl_val), 0, sizeof(repl_val));
                securec_check(rc, "", "");
                rc = memset_s(repl_null, sizeof(repl_null), false, sizeof(repl_null));
                securec_check(rc, "", "");
                rc = memset_s(repl_repl, sizeof(repl_repl), false, sizeof(repl_repl));
                securec_check(rc, "", "");

                repl_val[Anum_pg_ts_config_map_mapdict - 1] = ObjectIdGetDatum(dictNew);
                repl_repl[Anum_pg_ts_config_map_mapdict - 1] = true;

                newtup = (HeapTuple) tableam_tops_modify_tuple(maptup, RelationGetDescr(relMap), repl_val, repl_null, repl_repl);
                simple_heap_update(relMap, &newtup->t_self, newtup);

                CatalogUpdateIndexes(relMap, newtup);
            }
        }

        systable_endscan(scan);
    } else {
        /*
         * Insertion of new entries
         */
        for (i = 0; i < ntoken; i++) {
            for (j = 0; j < ndict; j++) {
                Datum values[Natts_pg_ts_config_map];
                bool nulls[Natts_pg_ts_config_map];

                errno_t rc = memset_s(nulls, sizeof(nulls), false, sizeof(nulls));
                securec_check(rc, "", "");
                values[Anum_pg_ts_config_map_mapcfg - 1] = ObjectIdGetDatum(cfgId);
                values[Anum_pg_ts_config_map_maptokentype - 1] = Int32GetDatum(tokens[i]);
                values[Anum_pg_ts_config_map_mapseqno - 1] = Int32GetDatum(j + 1);
                values[Anum_pg_ts_config_map_mapdict - 1] = ObjectIdGetDatum(dictIds[j]);

                tup = heap_form_tuple(relMap->rd_att, values, nulls);
                (void)simple_heap_insert(relMap, tup);
                CatalogUpdateIndexes(relMap, tup);

                tableam_tops_free_tuple(tup);
            }
        }
    }
}

/*
 * ALTER TEXT SEARCH CONFIGURATION DROP MAPPING
 */
static void DropConfigurationMapping(AlterTSConfigurationStmt* stmt, HeapTuple tup, Relation relMap)
{
    Oid cfgId = HeapTupleGetOid(tup);
    ScanKeyData skey[2];
    SysScanDesc scan;
    HeapTuple maptup;
    int i;
    Oid prsId;
    int* tokens = NULL;
    ListCell* c = NULL;

    prsId = ((Form_pg_ts_config)GETSTRUCT(tup))->cfgparser;

    tokens = getTokenTypes(prsId, stmt->tokentype);

    i = 0;
    foreach (c, stmt->tokentype) {
        Value* val = (Value*)lfirst(c);
        bool found = false;

        ScanKeyInit(&skey[0], Anum_pg_ts_config_map_mapcfg, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(cfgId));
        ScanKeyInit(
            &skey[1], Anum_pg_ts_config_map_maptokentype, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(tokens[i]));

        scan = systable_beginscan(relMap, TSConfigMapIndexId, true, SnapshotNow, 2, skey);

        while (HeapTupleIsValid((maptup = systable_getnext(scan)))) {
            simple_heap_delete(relMap, &maptup->t_self);
            found = true;
        }

        systable_endscan(scan);

        if (!found) {
            if (!stmt->missing_ok) {
                ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("mapping for token type \"%s\" does not exist", strVal(val))));
            } else {
                ereport(NOTICE, (errmsg("mapping for token type \"%s\" does not exist, skipping", strVal(val))));
            }
        }

        i++;
    }
}

/*
 * Serialize dictionary options, producing a TEXT datum from a List of DefElem
 *
 * This is used to form the value stored in pg_ts_dict.dictinitoption.
 * For the convenience of pg_dump, the output is formatted exactly as it
 * would need to appear in CREATE TEXT SEARCH DICTIONARY to reproduce the
 * same options.
 *
 * Note that we assume that only the textual representation of an option's
 * value is interesting --- hence, non-string DefElems get forced to strings.
 */
text* serialize_deflist(List* deflist)
{
    text* result = NULL;
    StringInfoData buf;
    ListCell* l = NULL;

    initStringInfo(&buf);

    foreach (l, deflist) {
        DefElem* defel = (DefElem*)lfirst(l);
        /* Skip FilePath */
        if (pg_strcasecmp("FilePath", defel->defname) == 0)
            continue;

        if (l != deflist->head)
            appendStringInfo(&buf, ", ");

        char* val = defGetString(defel);
        appendStringInfo(&buf, "%s = ", quote_identifier(defel->defname));
        /* If backslashes appear, force E syntax to determine their handling */
        if (strchr(val, '\\'))
            appendStringInfoChar(&buf, ESCAPE_STRING_SYNTAX);
        appendStringInfoChar(&buf, '\'');
        while (*val) {
            char ch = *val++;

            if (SQL_STR_DOUBLE(ch, true))
                appendStringInfoChar(&buf, ch);
            appendStringInfoChar(&buf, ch);
        }
        appendStringInfoChar(&buf, '\'');
    }

    result = cstring_to_text_with_len(buf.data, buf.len);
    pfree_ext(buf.data);
    return result;
}

/*
 * Deserialize dictionary options, reconstructing a List of DefElem from TEXT
 *
 * This is also used for prsheadline options, so for backward compatibility
 * we need to accept a few things serialize_deflist() will never emit:
 * in particular, unquoted and double-quoted values.
 */
List* deserialize_deflist(Datum txt)
{
    text* in = DatumGetTextP(txt); /* in case it's toasted */
    List* result = NIL;
    int len = VARSIZE(in) - VARHDRSZ;
    char* ptr = NULL;
    char* endptr = NULL;
    char* workspace = NULL;
    char* wsptr = NULL;
    char* startvalue = NULL;
    typedef enum {
        CS_WAITKEY,
        CS_INKEY,
        CS_INQKEY,
        CS_WAITEQ,
        CS_WAITVALUE,
        CS_INSQVALUE,
        CS_INDQVALUE,
        CS_INWVALUE
    } ds_state;
    ds_state state = CS_WAITKEY;

    workspace = (char*)palloc(len + 1); /* certainly enough room */
    ptr = VARDATA(in);
    endptr = ptr + len;
    for (; ptr < endptr; ptr++) {
        switch (state) {
            case CS_WAITKEY:
                if (isspace((unsigned char)*ptr) || *ptr == ',')
                    continue;
                if (*ptr == '"') {
                    wsptr = workspace;
                    state = CS_INQKEY;
                } else {
                    wsptr = workspace;
                    *wsptr++ = *ptr;
                    state = CS_INKEY;
                }
                break;
            case CS_INKEY:
                if (isspace((unsigned char)*ptr)) {
                    *wsptr++ = '\0';
                    state = CS_WAITEQ;
                } else if (*ptr == '=') {
                    *wsptr++ = '\0';
                    state = CS_WAITVALUE;
                } else {
                    *wsptr++ = *ptr;
                }
                break;
            case CS_INQKEY:
                if (*ptr == '"') {
                    if (ptr + 1 < endptr && ptr[1] == '"') {
                        /* copy only one of the two quotes */
                        *wsptr++ = *ptr++;
                    } else {
                        *wsptr++ = '\0';
                        state = CS_WAITEQ;
                    }
                } else {
                    *wsptr++ = *ptr;
                }
                break;
            case CS_WAITEQ:
                if (*ptr == '=') {
                    state = CS_WAITVALUE;
                } else if (!isspace((unsigned char)*ptr)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("invalid parameter list format: \"%s\"", text_to_cstring(in))));
                }
                break;
            case CS_WAITVALUE:
                if (*ptr == '\'') {
                    startvalue = wsptr;
                    state = CS_INSQVALUE;
                } else if (*ptr == 'E' && ptr + 1 < endptr && ptr[1] == '\'') {
                    ptr++;
                    startvalue = wsptr;
                    state = CS_INSQVALUE;
                } else if (*ptr == '"') {
                    startvalue = wsptr;
                    state = CS_INDQVALUE;
                } else if (!isspace((unsigned char)*ptr)) {
                    startvalue = wsptr;
                    *wsptr++ = *ptr;
                    state = CS_INWVALUE;
                }
                break;
            case CS_INSQVALUE:
                if (*ptr == '\'') {
                    if (ptr + 1 < endptr && ptr[1] == '\'') {
                        /* copy only one of the two quotes */
                        *wsptr++ = *ptr++;
                    } else {
                        *wsptr++ = '\0';
                        result =
                            lappend(result, makeDefElem(pstrdup(workspace), (Node*)makeString(pstrdup(startvalue))));
                        state = CS_WAITKEY;
                    }
                } else if (*ptr == '\\') {
                    if (ptr + 1 < endptr && ptr[1] == '\\') {
                        /* copy only one of the two backslashes */
                        *wsptr++ = *ptr++;
                    } else {
                        *wsptr++ = *ptr;
                    }
                } else {
                    *wsptr++ = *ptr;
                }
                break;
            case CS_INDQVALUE:
                if (*ptr == '"') {
                    if (ptr + 1 < endptr && ptr[1] == '"') {
                        /* copy only one of the two quotes */
                        *wsptr++ = *ptr++;
                    } else {
                        *wsptr++ = '\0';
                        result =
                            lappend(result, makeDefElem(pstrdup(workspace), (Node*)makeString(pstrdup(startvalue))));
                        state = CS_WAITKEY;
                    }
                } else {
                    *wsptr++ = *ptr;
                }
                break;
            case CS_INWVALUE:
                if (*ptr == ',' || isspace((unsigned char)*ptr)) {
                    *wsptr++ = '\0';
                    result = lappend(result, makeDefElem(pstrdup(workspace), (Node*)makeString(pstrdup(startvalue))));
                    state = CS_WAITKEY;
                } else {
                    *wsptr++ = *ptr;
                }
                break;
            default:
                ereport(ERROR,
                    (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmsg("unrecognized deserialize_deflist state: %d", state)));
        }
    }

    if (state == CS_INWVALUE) {
        *wsptr++ = '\0';
        result = lappend(result, makeDefElem(pstrdup(workspace), (Node*)makeString(pstrdup(startvalue))));
    } else if (state != CS_WAITKEY) {
        ereport(ERROR,
            (errcode(ERRCODE_SYNTAX_ERROR), errmsg("invalid parameter list format: \"%s\"", text_to_cstring(in))));
    }

    pfree_ext(workspace);

    return result;
}

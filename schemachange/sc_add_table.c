/*
   Copyright 2015 Bloomberg Finance L.P.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#include "schemachange.h"
#include "schemachange_int.h"
#include "sc_add_table.h"
#include "logmsg.h"

static inline int adjust_master_tables(struct db *newdb, const char *csc2,
                                       struct ireq *iq)
{
    int rc;
    int pi = 0; // partial indexes

    fix_lrl_ixlen(); /* nblava */
    /* fix_lrl_ixlen() usually sets csc2_schema/csc2_schema_len, but for llmeta
     * dbs, it grabs this info from llmeta and in the case where we are adding a
     * table the schema is not yet in llmeta
     * drqs 15824294 */
    if (!newdb->csc2_schema && csc2) {
        newdb->csc2_schema = strdup(csc2);
        newdb->csc2_schema_len = strlen(newdb->csc2_schema);
    }
    rc = create_sqlmaster_records(NULL);

    if (rc != 0) {
        logmsg(LOGMSG_ERROR, "create_sqlmaster_records failed rc %d\n", rc);
        return SC_INTERNAL_ERROR;
    }
    /* TODO: ask why this function has no return codes */
    create_master_tables(); /* create sql statements */

    extern int gbl_partial_indexes;
    extern int gbl_expressions_indexes;
    if (((gbl_partial_indexes && newdb->ix_partial) ||
         (gbl_expressions_indexes && newdb->ix_expr)) &&
        newdb->dbenv->master == gbl_mynode)
        rc = new_indexes_syntax_check(iq);

    if (rc)
        return SC_CSC2_ERROR;
    else
        return 0;
}

static inline int get_db_handle(struct db *newdb)
{
    int bdberr;
    if (newdb->dbenv->master == gbl_mynode) {
        /* I am master: create new db */
        newdb->handle = bdb_create(
            newdb->dbname, thedb->basedir, newdb->lrl, newdb->nix,
            newdb->ix_keylen, newdb->ix_dupes, newdb->ix_recnums,
            newdb->ix_datacopy, newdb->ix_collattr, newdb->ix_nullsallowed,
            newdb->numblobs + 1, thedb->bdb_env, 0, &bdberr);
        open_auxdbs(newdb, 1);
    } else {
        /* I am NOT master: open replicated db */
        newdb->handle = bdb_open_more(
            newdb->dbname, thedb->basedir, newdb->lrl, newdb->nix,
            newdb->ix_keylen, newdb->ix_dupes, newdb->ix_recnums,
            newdb->ix_datacopy, newdb->ix_collattr, newdb->ix_nullsallowed,
            newdb->numblobs + 1, thedb->bdb_env, &bdberr);
        open_auxdbs(newdb, 0);
    }

    if (newdb->handle == NULL) {
        logmsg(LOGMSG_ERROR, "bdb_open:failed to open table %s/%s, rcode %d\n",
               thedb->basedir, newdb->dbname, bdberr);
        return SC_BDB_ERROR;
    }

    return SC_OK;
}

static inline int init_bthashsize(struct db *newdb)
{
    int bthashsz;

    if (get_db_bthash(newdb, &bthashsz) != 0)
        bthashsz = 0;

    if (bthashsz) {
        logmsg(LOGMSG_INFO, "Init with bthash size %dkb per stripe\n", bthashsz);
        bdb_handle_dbp_add_hash(newdb->handle, bthashsz);
    }

    return SC_OK;
}

/* Add a new table.  Assume new table has no db number.
 * If csc2 is provided then a filename is chosen, the lrl updated and
 * the file written with the csc2. */
int add_table_to_environment(char *table, char *fname, const char *csc2,
                             const char *aname, struct schema_change_type *s,
                             struct ireq *iq)
{
    int rc;
    struct db *newdb;
    int bdberr;

    if (!csc2) {
        logmsg(LOGMSG_ERROR, "%s: no filename or csc2!\n", __func__);
        return -1;
    }

    rc = dyns_load_schema_string((char *)csc2, thedb->envname, table);

    if (rc) {
        char *err;
        char *syntax_err;
        err = csc2_get_errors();
        syntax_err = csc2_get_syntax_errors();
        if (iq)
            reqerrstr(iq, ERR_SC, "%s", syntax_err);
        sc_errf(s, "%s\n", err);
        sc_errf(s, "error adding new table locally\n");
        logmsg(LOGMSG_WARN, "Failed to load schema for table %s\n", table);
        logmsg(LOGMSG_WARN, "Dumping schema for reference: '%s'\n", csc2);
        return SC_CSC2_ERROR;
    }
    newdb = newdb_from_schema(thedb, table, fname, 0, thedb->num_dbs, 0);

    if (newdb == NULL)
        return SC_INTERNAL_ERROR;

    newdb->dtastripe = gbl_dtastripe;

    newdb->iq = iq;

    if (add_cmacc_stmt(newdb, 0)) {
        logmsg(LOGMSG_ERROR, "%s: add_cmacc_stmt failed\n", __func__);
        rc = SC_CSC2_ERROR;
        goto err;
    }

    if (verify_constraints_exist(newdb, NULL, NULL, s) != 0) {
        logmsg(LOGMSG_ERROR, "%s: Verify constraints failed \n", __func__);
        rc = -1;
        goto err;
    }

    if (populate_reverse_constraints(newdb)) {
        logmsg(LOGMSG_ERROR, "%s: populating reverse constraints failed\n", __func__);
        rc = -1;
        goto err;
    }

    if (!sc_via_ddl_only() && validate_ix_names(newdb)) {
        rc = -1;
        goto err;
    }

    if ((rc = get_db_handle(newdb)))
        goto err;

    gbl_sc_commit_count++;
    thedb->dbs =
        realloc(thedb->dbs, (thedb->num_dbs + 1) * sizeof(struct db *));
    thedb->dbs[thedb->num_dbs++] = newdb;

    rc = adjust_master_tables(newdb, csc2, iq);
    if (rc) {
        gbl_sc_commit_count--;
        thedb->dbs[--thedb->num_dbs] = NULL;
        if (rc == SC_CSC2_ERROR)
            sc_errf(s, "New indexes syntax error\n");
        goto err;
    }
    newdb->schema->ix_blob = newdb->ix_blob;

    if ((rc = write_csc2_file(newdb, csc2))) {
        rc = SC_INTERNAL_ERROR;
        goto err;
    }

    newdb->iq = NULL;
    init_bthashsize(newdb);

    return SC_OK;

err:
    newdb->iq = NULL;
    backout_schemas(newdb->dbname);
    cleanup_newdb(newdb);
    return rc;
}

static inline void set_empty_options(struct schema_change_type *s)
{
    if (s->headers == -1)
        s->headers = gbl_init_with_odh;
    if (s->compress == -1)
        s->compress = gbl_init_with_compr;
    if (s->compress_blobs == -1)
        s->compress_blobs = gbl_init_with_compr_blobs;
    if (s->ip_updates == -1)
        s->ip_updates = gbl_init_with_ipu;
    if (s->instant_sc == -1)
        s->instant_sc = gbl_init_with_instant_sc;
}

int finalize_add_table(struct schema_change_type *s);

int do_add_table_int(struct schema_change_type *s, struct ireq *iq)
{
    int rc = SC_OK;
    int bdberr;
    struct db *db;
    set_empty_options(s);

    if ((rc = check_option_coherency(s, NULL, NULL)))
        return rc;

    if ((db = getdbbyname(s->table))) {
        sc_errf(s, "Table already exists");
        logmsg(LOGMSG_ERROR, "Table already exists\n");

        return SC_TABLE_ALREADY_EXIST;
    }

    rc = add_table_to_environment(s->table, NULL, s->newcsc2, s->aname, s, iq);

    if (rc) {
        sc_errf(s, "error adding new table locally\n");
        return rc;
    }

    if (!(db = getdbbyname(s->table)))
        return SC_INTERNAL_ERROR;

    db->sc_to = s->db = db;
    db->odh = s->headers;
    db->inplace_updates = s->ip_updates;
    db->version = 1;

    /* compression algorithms set to 0 for new table - this
       will have to be changed manually by the operator */
    set_bdb_option_flags(db->handle, s->headers, s->ip_updates, s->instant_sc,
                         db->version, s->compress, s->compress_blobs, 1);

    return 0;
}

int finalize_add_table(struct schema_change_type *s)
{
    int rc, bdberr = 0;
    int retries = 0;
    struct ireq iq;
    struct db *db = s->db;
    void *tran = NULL;

    init_fake_ireq(thedb, &iq);
    iq.usedb = db;
retry_add_table:

    if (tran) /* if this is a retry and not the first pass */
    {
        trans_abort(&iq, tran);
        tran = NULL;

        sc_errf(s, "Adding table failed for %s rc %d bdberr %d "
                   "attempting retry\n",
                s->table, rc, bdberr);
    }

    if (++retries >= gbl_maxretries) {
        sc_errf(s, "Adding table failed giving up after %d retries\n", retries);
        return SC_TRANSACTION_FAILED;
    }

    rc = trans_start_sc(&iq, NULL, &tran);
    if (rc) {
        sc_errf(s, "Failed starting add table transaction\n");
        goto retry_add_table;
    }

    sc_printf(s, "Start add table transaction ok\n");

    rc = load_new_table_schema_tran(thedb, tran, s->table, s->newcsc2);
    if (rc != 0) {
        sc_errf(s, "error recording new table schema\n");
        goto retry_add_table;
    }

    /* Set instant schema-change */
    db->instant_schema_change = db->odh && s->instant_sc;

    if ((rc = set_header_and_properties(tran, db, s, 0, gbl_init_with_bthash)))
        goto retry_add_table;

    if (llmeta_set_tables(tran, thedb)) {
        sc_errf(s, "Failed to set table names in low level meta\n");
        goto retry_add_table;
    }

    if (bdb_table_version_select(db->handle, tran, &db->tableversion,
                                 &bdberr)) {
        sc_errf(s, "Failed fetching table version bdberr %d\n", bdberr);
        goto retry_add_table;
    }

    if ((rc = mark_schemachange_over(tran, db->dbname)))
        goto retry_add_table;

    /* Save .ONDISK as schema version 1 if instant_sc is enabled. */
    if (db->odh && db->instant_schema_change) {
        struct schema *ver_one;
        if ((rc = prepare_table_version_one(tran, db, &ver_one)))
            goto retry_add_table;
        add_tag_schema(db->dbname, ver_one);
    }

    create_sqlmaster_records(tran);
    create_master_tables();
    rc = trans_commit_adaptive(&iq, tran, gbl_mynode);
    if (rc) {
        sc_errf(s, "Failed add table commit\n");
        goto retry_add_table;
    }

    tran = NULL;
    db->sc_to = NULL;
    update_dbstore(db);
    sc_printf(s, "Add table ok\n");

    fix_lrl_ixlen();

    if (gbl_init_with_bthash) {
        logmsg(LOGMSG_INFO, "Init table with bthash size %dkb per stripe\n",
               gbl_init_with_bthash);
        if (put_db_bthash(db, NULL, gbl_init_with_bthash) != 0) {
            logmsg(LOGMSG_ERROR, "Failed to write bthash to meta table\n");
            return -1;
        }
        bdb_handle_dbp_add_hash(db->handle, gbl_init_with_bthash);
    }

    /* this is the custom log record to tell replicants to add the
     * table*/
    if ((rc = bdb_llog_scdone(db->handle, add, 1, &bdberr)) ||
        bdberr != BDBERR_NOERROR) {
        sc_errf(s, "Failed to send logical log scdone rc=%d bdberr=%d\n", rc,
                bdberr);
        return -1;
    }

    sc_printf(s, "Schema change ok\n");
    return 0;
}

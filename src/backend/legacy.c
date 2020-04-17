/**
 * Create a taler://pay/ URI for the given @a con and @a order_id
 * and @a session_id and @a instance_id.
 *
 * @param con HTTP connection
 * @param order_id the order id
 * @param session_id session, may be NULL
 * @param instance_id instance, may be "default"
 * @return corresponding taler://pay/ URI, or NULL on missing "host"
 */
char *
TMH_make_taler_pay_uri (struct MHD_Connection *con,
                        const char *order_id,
                        const char *session_id,
                        const char *instance_id)
{
  const char *host;
  const char *forwarded_host;
  const char *uri_path;
  const char *uri_instance_id;
  const char *query;
  char *result;

  host = MHD_lookup_connection_value (con,
                                      MHD_HEADER_KIND,
                                      "Host");
  forwarded_host = MHD_lookup_connection_value (con,
                                                MHD_HEADER_KIND,
                                                "X-Forwarded-Host");

  uri_path = MHD_lookup_connection_value (con,
                                          MHD_HEADER_KIND,
                                          "X-Forwarded-Prefix");
  if (NULL == uri_path)
    uri_path = "-";
  if (NULL != forwarded_host)
    host = forwarded_host;
  if (0 == strcmp (instance_id,
                   "default"))
    uri_instance_id = "-";
  else
    uri_instance_id = instance_id;
  if (NULL == host)
  {
    /* Should never happen, at least the host header should be defined */
    GNUNET_break (0);
    return NULL;
  }

  if (GNUNET_YES == TALER_mhd_is_https (con))
    query = "";
  else
    query = "?insecure=1";
  GNUNET_assert (NULL != order_id);
  GNUNET_assert (0 < GNUNET_asprintf (&result,
                                      "taler://pay/%s/%s/%s/%s%s%s%s",
                                      host,
                                      uri_path,
                                      uri_instance_id,
                                      order_id,
                                      (NULL == session_id) ? "" : "/",
                                      (NULL == session_id) ? "" : session_id,
                                      query));
  return result;
}


/**
 * Closure for the #wireformat_iterator_cb().
 */
struct WireFormatIteratorContext
{
  /**
   * The global iteration context.
   */
  struct IterateInstancesCls *iic;

  /**
   * The merchant instance we are currently building.
   */
  struct MerchantInstance *mi;

  /**
   * Set to #GNUNET_YES if the default instance was found.
   */
  int default_instance;
};


/**
 * Callback that looks for 'merchant-account-*' sections,
 * and populates our wire method according to the data
 *
 * @param cls closure with a `struct WireFormatIteratorContext *`
 * @section section name this callback gets
 */
static void
wireformat_iterator_cb (void *cls,
                        const char *section)
{
  struct WireFormatIteratorContext *wfic = cls;
  struct MerchantInstance *mi = wfic->mi;
  struct IterateInstancesCls *iic = wfic->iic;
  char *instance_option;
  struct WireMethod *wm;
  char *payto;
  char *fn;
  json_t *j;
  struct GNUNET_HashCode jh_wire;
  char *wire_file_mode;

  if (0 != strncasecmp (section,
                        "merchant-account-",
                        strlen ("merchant-account-")))
    return;
  GNUNET_asprintf (&instance_option,
                   "HONOR_%s",
                   mi->id);
  if (GNUNET_YES !=
      GNUNET_CONFIGURATION_get_value_yesno (cfg,
                                            section,
                                            instance_option))
  {
    GNUNET_free (instance_option);
    return;
  }
  GNUNET_free (instance_option);
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "PAYTO_URI",
                                             &payto))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "PAYTO_URI");
    iic->ret = GNUNET_SYSERR;
    return;
  }

  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_filename (cfg,
                                               section,
                                               "WIRE_RESPONSE",
                                               &fn))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "WIRE_RESPONSE");
    GNUNET_free (payto);
    iic->ret = GNUNET_SYSERR;
    return;
  }

  /* Try loading existing JSON from file */
  if (GNUNET_YES ==
      GNUNET_DISK_file_test (fn))
  {
    json_error_t err;
    char *url;

    if (NULL ==
        (j = json_load_file (fn,
                             JSON_REJECT_DUPLICATES,
                             &err)))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to load JSON from `%s': %s at %d:%d\n",
                  fn,
                  err.text,
                  err.line,
                  err.column);
      GNUNET_free (fn);
      GNUNET_free (payto);
      iic->ret = GNUNET_SYSERR;
      return;
    }
    url = TALER_JSON_wire_to_payto (j);
    if (NULL == url)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "URL missing in `%s', disabling account `%s'\n",
                  fn,
                  section);
      GNUNET_free (fn);
      GNUNET_free (payto);
      iic->ret = GNUNET_SYSERR;
      return;
    }
    if (0 != strcasecmp (url,
                         payto))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "URL `%s' does not match configuration `%s', disabling account `%s'\n",
                  url,
                  payto,
                  section);
      GNUNET_free (fn);
      GNUNET_free (payto);
      GNUNET_free (url);
      iic->ret = GNUNET_SYSERR;
      return;
    }
    GNUNET_free (url);
  }
  else /* need to generate JSON */
  {
    struct GNUNET_HashCode salt;
    char *salt_str;

    GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_NONCE,
                                &salt,
                                sizeof (salt));
    salt_str = GNUNET_STRINGS_data_to_string_alloc (&salt,
                                                    sizeof (salt));
    j = json_pack ("{s:s, s:s}",
                   "payto_uri", payto,
                   "salt", salt_str);
    GNUNET_free (salt_str);

    /* Make sure every path component exists.  */
    if (GNUNET_OK != GNUNET_DISK_directory_create_for_file (fn))
    {
      GNUNET_log_strerror_file (GNUNET_ERROR_TYPE_ERROR,
                                "mkdir",
                                fn);
      GNUNET_free (fn);
      GNUNET_free (payto);
      json_decref (j);
      iic->ret = GNUNET_SYSERR;
      return;
    }

    if (0 != json_dump_file (j,
                             fn,
                             JSON_COMPACT | JSON_SORT_KEYS))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to write hashed wire details to `%s'\n",
                  fn);
      GNUNET_free (fn);
      GNUNET_free (payto);
      json_decref (j);
      iic->ret = GNUNET_SYSERR;
      return;
    }

    if (GNUNET_OK ==
        GNUNET_CONFIGURATION_get_value_string (cfg,
                                               section,
                                               "WIRE_FILE_MODE",
                                               &wire_file_mode))
    {
      errno = 0;
      mode_t mode = (mode_t) strtoul (wire_file_mode,
                                      NULL,
                                      8);
      if (0 != errno)
      {
        GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                   section,
                                   "WIRE_FILE_MODE",
                                   "Must be octal number\n");
        iic->ret = GNUNET_SYSERR;
        GNUNET_free (fn);
        return;
      }
      if (0 != chmod (fn, mode))
      {
        TALER_LOG_ERROR ("chmod failed on %s\n", fn);
        iic->ret = GNUNET_SYSERR;
        GNUNET_free (fn);
        return;
      }
    }
  }

  GNUNET_free (fn);

  if (GNUNET_OK !=
      TALER_JSON_merchant_wire_signature_hash (j,
                                               &jh_wire))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to hash wire input\n");
    GNUNET_free (fn);
    GNUNET_free (payto);
    json_decref (j);
    iic->ret = GNUNET_SYSERR;
    return;
  }

  wm = GNUNET_new (struct WireMethod);
  wm->wire_method = TALER_payto_get_method (payto);
  GNUNET_free (payto);
  GNUNET_asprintf (&instance_option,
                   "ACTIVE_%s",
                   mi->id);
  wm->active = GNUNET_CONFIGURATION_get_value_yesno (cfg,
                                                     section,
                                                     instance_option);
  GNUNET_free (instance_option);
  if (GNUNET_YES == wm->active)
    GNUNET_CONTAINER_DLL_insert (mi->wm_head,
                                 mi->wm_tail,
                                 wm);
  else
    GNUNET_CONTAINER_DLL_insert_tail (mi->wm_head,
                                      mi->wm_tail,
                                      wm);
  wm->j_wire = j;
  wm->h_wire = jh_wire;
}


/**
 * Callback that looks for 'instance-*' sections,
 * and populates accordingly each instance's data
 *
 * @param cls closure of type `struct IterateInstancesCls`
 * @section section name this callback gets
 */
static void
instances_iterator_cb (void *cls,
                       const char *section)
{
  struct IterateInstancesCls *iic = cls;
  char *token;
  struct MerchantInstance *mi;
  /* used as hashmap keys */
  struct GNUNET_HashCode h_pk;
  struct GNUNET_HashCode h_id;

  if (0 != strncasecmp (section,
                        "instance-",
                        strlen ("instance-")))
    return;
  /** Get id **/
  token = strrchr (section, '-');
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Extracted token: %s\n",
              token + 1);
  mi = GNUNET_new (struct MerchantInstance);
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "NAME",
                                             &mi->name))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "NAME");
    GNUNET_free (mi);
    iic->ret = GNUNET_SYSERR;
    return;
  }

  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_filename (cfg,
                                               section,
                                               "KEYFILE",
                                               &mi->keyfile))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "KEYFILE");
    GNUNET_free (mi->name);
    GNUNET_free (mi);
    iic->ret = GNUNET_SYSERR;
    return;
  }
  if (GNUNET_OK ==
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "TIP_EXCHANGE",
                                             &mi->tip_exchange))
  {
    char *tip_reserves;

    if (GNUNET_OK !=
        GNUNET_CONFIGURATION_get_value_filename (cfg,
                                                 section,
                                                 "TIP_RESERVE_PRIV_FILENAME",
                                                 &tip_reserves))
    {
      GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                                 section,
                                 "TIP_RESERVE_PRIV_FILENAME");
      GNUNET_free (mi->keyfile);
      GNUNET_free (mi->name);
      GNUNET_free (mi);
      iic->ret = GNUNET_SYSERR;
      return;
    }
    if (GNUNET_OK !=
        GNUNET_CRYPTO_eddsa_key_from_file (tip_reserves,
                                           GNUNET_NO,
                                           &mi->tip_reserve.eddsa_priv))
    {
      GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                 section,
                                 "TIP_RESERVE_PRIV_FILENAME",
                                 "Failed to read private key");
      GNUNET_free (tip_reserves);
      GNUNET_free (mi->keyfile);
      GNUNET_free (mi->name);
      GNUNET_free (mi);
      iic->ret = GNUNET_SYSERR;
      return;
    }
    GNUNET_free (tip_reserves);
  }

  if (GNUNET_SYSERR ==
      GNUNET_CRYPTO_eddsa_key_from_file (mi->keyfile,
                                         GNUNET_YES,
                                         &mi->privkey.eddsa_priv))
  {
    GNUNET_break (0);
    GNUNET_free (mi->keyfile);
    GNUNET_free (mi->name);
    GNUNET_free (mi);
    iic->ret = GNUNET_SYSERR;
    return;
  }
  GNUNET_CRYPTO_eddsa_key_get_public (&mi->privkey.eddsa_priv,
                                      &mi->pubkey.eddsa_pub);

  mi->id = GNUNET_strdup (token + 1);
  if (0 == strcasecmp ("default",
                       mi->id))
    iic->default_instance = GNUNET_YES;

  GNUNET_CRYPTO_hash (mi->id,
                      strlen (mi->id),
                      &h_id);
  if (GNUNET_OK !=
      GNUNET_CONTAINER_multihashmap_put (by_id_map,
                                         &h_id,
                                         mi,
                                         GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to put an entry into the 'by_id' hashmap\n");
    iic->ret = GNUNET_SYSERR;
    GNUNET_free (mi->keyfile);
    GNUNET_free (mi->name);
    GNUNET_free (mi);
    return;
  }
  GNUNET_CRYPTO_hash (&mi->pubkey.eddsa_pub,
                      sizeof (struct GNUNET_CRYPTO_EddsaPublicKey),
                      &h_pk);


  /* Initialize wireformats */
  {
    struct WireFormatIteratorContext wfic = {
      .iic = iic,
      .mi = mi
    };

    GNUNET_CONFIGURATION_iterate_sections (cfg,
                                           &wireformat_iterator_cb,
                                           &wfic);
  }
  if (NULL == mi->wm_head)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to load wire formats for instance `%s'\n",
                mi->id);
    iic->ret = GNUNET_SYSERR;
  }

}


/**
 * Iterate over each merchant instance, in order to populate
 * each instance's own data
 *
 * @return #GNUNET_OK if successful, #GNUNET_SYSERR upon errors
 *          (for example, if no "default" instance is defined)
 */
static int
iterate_instances (void)
{
  struct IterateInstancesCls iic;

  iic.default_instance = GNUNET_NO;
  iic.ret = GNUNET_OK;
  GNUNET_CONFIGURATION_iterate_sections (cfg,
                                         &instances_iterator_cb,
                                         &iic);

  if (GNUNET_NO == iic.default_instance)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "No default merchant instance found\n");
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK != iic.ret)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "At least one instance was not successfully parsed\n");
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}

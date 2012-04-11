// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "simulationcraft.hpp"

#ifdef SC_SIGACTION
#include <signal.h>
#endif

namespace { // ANONYMOUS NAMESPACE ==========================================

#ifdef SC_SIGACTION
// POSIX-only signal handler ================================================

struct sim_signal_handler_t
{
  static sim_t* global_sim;

  static void sigint( int )
  {
    if ( global_sim )
    {
      if ( global_sim -> canceled ) exit( 0 );
      global_sim -> cancel();
    }
  }

  static void callback( int signal )
  {
    if ( global_sim )
    {
      const char* name = strsignal( signal );
      fprintf( stderr, "sim_signal_handler: %s! Seed=%d Iteration=%d\n",
               name, global_sim -> seed, global_sim -> current_iteration );
      fflush( stderr );
    }
    exit( 0 );
  }

  sim_signal_handler_t( sim_t* sim )
  {
    assert ( ! global_sim );
    global_sim = sim;
    struct sigaction sa;
    sigemptyset( &sa.sa_mask );
    sa.sa_flags = 0;

    sa.sa_handler = callback;
    sigaction( SIGSEGV, &sa, 0 );

    sa.sa_handler = sigint;
    sigaction( SIGINT,  &sa, 0 );
  }

  ~sim_signal_handler_t()
  { global_sim = 0; }
};
sim_t* sim_signal_handler_t::global_sim = 0;
#else
struct sim_signal_handler_t
{
  sim_signal_handler_t( sim_t* ) {}
};
#endif

// need_to_save_profiles ====================================================

static bool need_to_save_profiles( sim_t* sim )
{
  if ( sim -> save_profiles ) return true;

  for ( player_t* p = sim -> player_list; p; p = p -> next )
    if ( ! p -> report_information.save_str.empty() )
      return true;

  return false;
}

// parse_ptr ================================================================

static bool parse_ptr( sim_t*             sim,
                       const std::string& name,
                       const std::string& value )
{
  if ( name != "ptr" ) return false;

  if ( SC_USE_PTR )
    sim -> dbc.ptr = atoi( value.c_str() ) != 0;
  else
    sim -> errorf( "SimulationCraft has not been built with PTR data.  The 'ptr=' option is ignored.\n" );

  return true;
}

// parse_active =============================================================

static bool parse_active( sim_t*             sim,
                          const std::string& name,
                          const std::string& value )
{
  if ( name != "active" ) return false;

  if ( value == "owner" )
  {
    sim -> active_player = sim -> active_player -> cast_pet() -> owner;
  }
  else if ( value == "none" || value == "0" )
  {
    sim -> active_player = 0;
  }
  else
  {
    if ( sim -> active_player )
    {
      sim -> active_player = sim -> active_player -> find_pet( value );
    }
    if ( ! sim -> active_player )
    {
      sim -> active_player = sim -> find_player( value );
    }
    if ( ! sim -> active_player )
    {
      sim -> errorf( "Unable to find player %s to make active.\n", value.c_str() );
      return false;
    }
  }

  return true;
}

// parse_optimal_raid =======================================================

static bool parse_optimal_raid( sim_t*             sim,
                                const std::string& name,
                                const std::string& value )
{
  if ( name != "optimal_raid" ) return false;

  sim -> use_optimal_buffs_and_debuffs( atoi( value.c_str() ) );

  return true;
}

// parse_player =============================================================

static bool parse_player( sim_t*             sim,
                          const std::string& name,
                          const std::string& value )
{
  if ( name == "pet" )
  {
    std::string::size_type cut_pt = value.find( ',' );
    std::string pet_type( value, 0, cut_pt );

    std::string pet_name;
    if ( cut_pt != value.npos )
      pet_name.assign( value, cut_pt + 1, value.npos );
    else
      pet_name = value;

    sim -> active_player = sim -> active_player -> create_pet( pet_name, pet_type );
  }

  else if ( name == "copy" )
  {
    std::string::size_type cut_pt = value.find( ',' );
    std::string player_name( value, 0, cut_pt );

    player_t* source;
    if ( cut_pt == value.npos )
      source = sim -> active_player;
    else
      source = sim -> find_player( value.substr( cut_pt + 1 ) );

    if ( source == 0 )
    {
      sim -> errorf( "Invalid source for profile copy - format is copy=target[,source], source defaults to active player." );
      return false;
    }

    sim -> active_player = player_t::create( sim, util_t::player_type_string( source -> type ), player_name );
    if ( sim -> active_player != 0 ) sim -> active_player -> copy_from ( source );
  }

  else
    sim -> active_player = player_t::create( sim, name, value );

  return sim -> active_player != 0;
}

// parse_proxy ==============================================================

static bool parse_proxy( sim_t*             sim,
                         const std::string& /* name */,
                         const std::string& value )
{

  std::vector<std::string> splits;
  int num_splits = util_t::string_split( splits, value, "," );

  if ( num_splits != 3 )
  {
    sim -> errorf( "Expected format is: proxy=type,host,port\n" );
    return false;
  }

  int port = atoi( splits[ 2 ].c_str() );
  if ( splits[ 0 ] == "http" && port > 0 && port < 65536 )
  {
    http_t::proxy.type = splits[ 0 ];
    http_t::proxy.host = splits[ 1 ];
    http_t::proxy.port = port;
    return true;
  }

  return false;
}

// parse_cache ==============================================================

static bool parse_cache( sim_t*             /* sim */,
                         const std::string& name,
                         const std::string& value )
{
  if ( name == "cache_players" )
  {
    if ( value == "1" ) cache::players( cache::ANY );
    else if ( value == "0" ) cache::players( cache::CURRENT );
    else if ( util_t::str_compare_ci( value, "only" ) ) cache::players( cache::ONLY );
    else return false;

    return true;
  }

  else if ( name == "cache_items" )
  {
    if ( value == "1" ) cache::items( cache::ANY );
    else if ( value == "0" ) cache::items( cache::CURRENT );
    else if ( util_t::str_compare_ci( value, "only" ) ) cache::items( cache::ONLY );
    else return false;

    return true;
  }

  else
    return false;


  return true;
}

// parse_armory =============================================================

class names_and_options_t
{
private:
  bool is_valid_region( const std::string& s )
  { return s.size() == 2; }

public:
  struct error {};
  struct option_error : public error {};

  std::vector<std::string> names;
  std::string region;
  std::string server;
  cache::behavior_e cache;

  names_and_options_t( sim_t* sim, const std::string& context,
                       const option_t* client_options, const std::string& input )
  {
    int use_cache = 0;

    option_t base_options[] =
    {
      { "region", OPT_STRING, &region    },
      { "server", OPT_STRING, &server    },
      { "cache",  OPT_BOOL,   &use_cache },
    };

    std::vector<option_t> options;
    option_t::merge( options, base_options, client_options );

    size_t n = util_t::string_split( names, input, "," );
    size_t count = 0;
    for ( size_t i = 0; i < n; ++i )
    {
      if ( names[ i ].find( '=' ) != std::string::npos )
      {
        if ( unlikely( ! option_t::parse( sim, context.c_str(), options, names[ i ] ) ) )
          throw option_error();
      }
      else if ( count != i )
        names[ count++ ].swap( names[ i ] );
    }

    names.resize( count );

    if ( region.empty() )
    {
      if ( names.size() > 2 && is_valid_region( names[ 0 ] ) )
      {
        region = names[ 0 ];
        server = names[ 1 ];
        names.erase( names.begin(), names.begin() + 2 );
      }
      else
      {
        region = sim -> default_region_str;
        server = sim -> default_server_str;
      }
    }

    cache = use_cache ? cache::ANY : cache::players();
  }
};

bool parse_armory( sim_t*             sim,
                   const std::string& name,
                   const std::string& value )
{
  try
  {
    std::string spec = "active";

    option_t options[] =
    {
      { "spec", OPT_STRING,  &spec },
      { NULL,   OPT_UNKNOWN, NULL }
    };

    names_and_options_t stuff( sim, name, options, value );

    for ( size_t i = 0; i < stuff.names.size(); ++i )
    {
      // Format: name[|spec]
      std::string& player_name = stuff.names[ i ];
      std::string description = spec;

      if ( player_name[ 0 ] == '!' )
      {
        player_name.erase( 0, 1 );
        description = "inactive";
        sim -> errorf( "Warning: use of \"!%s\" to indicate a player's inactive talent spec is deprecated. Use \"%s|inactive\" instead.\n",
                       player_name.c_str(), player_name.c_str() );
      }

      std::string::size_type pos = player_name.find( '|' );
      if ( pos != player_name.npos )
      {
        description.assign( player_name, pos + 1, player_name.npos );
        player_name.erase( pos );
      }

      if ( ! sim -> input_is_utf8 )
        sim -> input_is_utf8 = range::is_valid_utf8( player_name ) && range::is_valid_utf8( stuff.server );

      player_t* p;
      if ( name == "wowhead" )
        p = wowhead_t::download_player( sim, stuff.region, stuff.server,
                                        player_name, description, stuff.cache );
      else if ( name == "chardev" )
        p = chardev_t::download_player( sim, player_name, stuff.cache );
      else if ( name == "wowreforge" )
        p = wowreforge::download_player( sim, player_name, stuff.cache );
      else
        p = bcp_api::download_player( sim, stuff.region, stuff.server,
                                      player_name, description, stuff.cache );

      sim -> active_player = p;
      if ( ! p )
        return false;
    }
  }

  catch ( names_and_options_t::error& )
  { return false; }

  return true;
}

bool parse_guild( sim_t*             sim,
                  const std::string& name,
                  const std::string& value )
{
  // Save Raid Summary file when guilds are downloaded
  sim -> save_raid_summary = 1;

  try
  {
    std::string type_str;
    std::string ranks_str;
    int max_rank = 0;

    option_t options[] =
    {
      { "class",    OPT_STRING,  &type_str  },
      { "max_rank", OPT_INT,     &max_rank  },
      { "ranks",    OPT_STRING,  &ranks_str },
      { NULL,       OPT_UNKNOWN, NULL }
    };

    names_and_options_t stuff( sim, name, options, value );

    std::vector<int> ranks_list;
    if ( ! ranks_str.empty() )
    {
      std::vector<std::string> ranks;
      int n_ranks = util_t::string_split( ranks, ranks_str, "/" );
      if ( n_ranks > 0 )
      {
        for ( int i = 0; i < n_ranks; i++ )
          ranks_list.push_back( atoi( ranks[i].c_str() ) );
      }
    }

    player_type_e pt = PLAYER_NONE;
    if ( ! type_str.empty() )
      pt = util_t::parse_player_type( type_str );

    for ( size_t i = 0; i < stuff.names.size(); ++i )
    {
      const std::string& guild_name = stuff.names[ i ];
      sim -> input_is_utf8 = range::is_valid_utf8( guild_name ) && range::is_valid_utf8( stuff.server );
      if ( ! bcp_api::download_guild( sim, stuff.region, stuff.server, guild_name,
                                      ranks_list, pt, max_rank, stuff.cache ) )
        return false;
    }
  }

  catch ( names_and_options_t::error& )
  { return false; }

  return true;
}

// parse_rawr ===============================================================

static bool parse_rawr( sim_t*             sim,
                        const std::string& name,
                        const std::string& value )
{
  if ( name == "rawr" )
  {
    sim -> active_player = rawr_t::load_player( sim, value );
    if ( ! sim -> active_player )
    {
      sim -> errorf( "Unable to parse Rawr Character Save file '%s'\n", value.c_str() );
    }
  }

  return sim -> active_player != 0;
}

// parse_fight_style ========================================================

static bool parse_fight_style( sim_t*             sim,
                               const std::string& name,
                               const std::string& value )
{
  if ( name != "fight_style" ) return false;

  if ( util_t::str_compare_ci( value, "Patchwerk" ) )
  {
    sim -> fight_style = "Patchwerk";
    sim -> raid_events_str.clear();
  }
  else if ( util_t::str_compare_ci( value, "Ultraxion" ) )
  {
    sim -> fight_style = "Ultraxion";
    sim -> max_time    = timespan_t::from_seconds( 366.0 );
    sim -> fixed_time  = 1;
    sim -> vary_combat_length = 0.0;
    sim -> raid_events_str =  "flying,first=0,duration=500,cooldown=500";
    sim -> raid_events_str +=  "/position_switch,first=0,duration=500,cooldown=500";
    sim -> raid_events_str += "/stun,duration=1.0,first=45.0,period=45.0";
    sim -> raid_events_str += "/stun,duration=1.0,first=57.0,period=57.0";
    sim -> raid_events_str += "/damage,first=6.0,period=6.0,last=59.5,amount=44000,type=shadow";
    sim -> raid_events_str += "/damage,first=60.0,period=5.0,last=119.5,amount=44855,type=shadow";
    sim -> raid_events_str += "/damage,first=120.0,period=4.0,last=179.5,amount=44855,type=shadow";
    sim -> raid_events_str += "/damage,first=180.0,period=3.0,last=239.5,amount=44855,type=shadow";
    sim -> raid_events_str += "/damage,first=240.0,period=2.0,last=299.5,amount=44855,type=shadow";
    sim -> raid_events_str += "/damage,first=300.0,period=1.0,amount=44855,type=shadow";
  }
  else if ( util_t::str_compare_ci( value, "HelterSkelter" ) )
  {
    sim -> fight_style = "HelterSkelter";
    sim -> raid_events_str = "casting,cooldown=30,duration=3,first=15";
    sim -> raid_events_str += "/movement,cooldown=30,duration=5";
    sim -> raid_events_str += "/stun,cooldown=60,duration=2";
    sim -> raid_events_str += "/invulnerable,cooldown=120,duration=3";
  }
  else if ( util_t::str_compare_ci( value, "LightMovement" ) )
  {
    sim -> fight_style = "LightMovement";
    sim -> raid_events_str = "/movement,players_only=1,first=";
    sim -> raid_events_str += util_t::to_string( int( sim -> max_time.total_seconds() * 0.1 ) );
    sim -> raid_events_str += ",cooldown=85,duration=7,last=";
    sim -> raid_events_str += util_t::to_string( int( sim -> max_time.total_seconds() * 0.8 ) );
  }
  else if ( util_t::str_compare_ci( value, "HeavyMovement" ) )
  {
    sim -> fight_style = "HeavyMovement";
    sim -> raid_events_str = "/movement,players_only=1,first=10,cooldown=10,duration=4";
  }
  else
  {
    log_t::output( sim, "Custom fight style specified: %s", value.c_str() );
    sim -> fight_style = value;
  }

  return true;
}

// parse_spell_query ========================================================

static bool parse_spell_query( sim_t*             sim,
                               const std::string& /* name */,
                               const std::string& value )
{
  std::string sq_str = value;
  size_t lvl_offset = std::string::npos;

  if ( ( lvl_offset = value.rfind( "@" ) ) != std::string::npos )
  {
    std::string lvl_offset_str = value.substr( lvl_offset + 1 );
    int sq_lvl = strtol( lvl_offset_str.c_str(), 0, 10 );
    if ( sq_lvl < 1 || sq_lvl > MAX_LEVEL )
      return 0;

    sim -> spell_query_level = static_cast< unsigned >( sq_lvl );

    sq_str = sq_str.substr( 0, lvl_offset );
  }

  sim -> spell_query = spell_data_expr_t::parse( sim, sq_str );
  return sim -> spell_query != 0;
}

// parse_item_sources =======================================================

static bool parse_item_sources( sim_t*             sim,
                                const std::string& /* name */,
                                const std::string& value )
{
  std::vector<std::string> sources;

  util_t::string_split( sources, value, ":/|", false );

  sim -> item_db_sources.clear();

  for ( unsigned i = 0; i < sources.size(); i++ )
  {
    if ( ! util_t::str_compare_ci( sources[ i ], "local" ) &&
         ! util_t::str_compare_ci( sources[ i ], "mmoc" ) &&
         ! util_t::str_compare_ci( sources[ i ], "wowhead" ) &&
         ! util_t::str_compare_ci( sources[ i ], "ptrhead" ) &&
         ! util_t::str_compare_ci( sources[ i ], "armory" ) &&
         ! util_t::str_compare_ci( sources[ i ], "bcpapi" ) )
    {
      continue;
    }

    sim -> item_db_sources.push_back( armory_t::format( sources[ i ] ) );
  }

  if ( sim -> item_db_sources.empty() )
  {
    sim -> errorf( "Your global data source string \"%s\" contained no valid data sources. Valid identifiers are: local, mmoc, wowhead, ptrhead and armory.\n",
                   value.c_str() );
    return false;
  }

  return true;
}

} // ANONYMOUS NAMESPACE ===================================================

// ==========================================================================
// Simulator
// ==========================================================================

// sim_t::sim_t =============================================================

sim_t::sim_t( sim_t* p, int index ) :
  parent( p ),
  target_list( 0 ), player_list( 0 ), active_player( 0 ), num_players( 0 ), num_enemies( 0 ), num_targetdata_ids( 0 ), max_player_level( -1 ), canceled( 0 ),
  queue_lag( timespan_t::from_seconds( 0.037 ) ), queue_lag_stddev( timespan_t::zero() ),
  gcd_lag( timespan_t::from_seconds( 0.150 ) ), gcd_lag_stddev( timespan_t::zero() ),
  channel_lag( timespan_t::from_seconds( 0.250 ) ), channel_lag_stddev( timespan_t::zero() ),
  queue_gcd_reduction( timespan_t::from_seconds( 0.032 ) ), strict_gcd_queue( 0 ),
  confidence( 0.95 ), confidence_estimator( 0.0 ),
  world_lag( timespan_t::from_seconds( 0.1 ) ), world_lag_stddev( timespan_t::min() ),
  travel_variance( 0 ), default_skill( 1.0 ), reaction_time( timespan_t::from_seconds( 0.5 ) ), regen_periodicity( timespan_t::from_seconds( 0.25 ) ),
  current_time( timespan_t::zero() ), max_time( timespan_t::from_seconds( 450 ) ), expected_time( timespan_t::zero() ), vary_combat_length( 0.2 ),
  last_event( timespan_t::zero() ), fixed_time( 0 ),
  events_remaining( 0 ), max_events_remaining( 0 ),
  events_processed( 0 ), total_events_processed( 0 ),
  seed( 0 ), id( 0 ), iterations( 1000 ), current_iteration( -1 ), current_slot( -1 ),
  armor_update_interval( 20 ), weapon_speed_scale_factors( 0 ),
  optimal_raid( 0 ), log( 0 ), debug( 0 ), save_profiles( 0 ), default_actions( 0 ),
  normalized_stat( STAT_NONE ),
  default_region_str( "us" ),
  save_prefix_str( "save_" ),
  save_talent_str( 0 ),
  input_is_utf8( false ),
  dtr_proc_chance( -1.0 ),
  target_death_pct( 0 ), target_level( -1 ), target_adds( 0 ),
  default_rng_( 0 ), rng_list( 0 ), deterministic_rng( false ),
  rng( 0 ), _deterministic_rng( 0 ), separated_rng( false ), average_range( true ), average_gauss( false ),
  convergence_scale( 2 ),
  timing_wheel( 0 ), wheel_seconds( 0 ), wheel_size( 0 ), wheel_mask( 0 ), timing_slice( 0 ), wheel_granularity( 0.0 ),
  fight_style( "Patchwerk" ), overrides( overrides_t() ), auras( auras_t() ),
  buff_list( 0 ), aura_delay( timespan_t::from_seconds( 0.5 ) ), default_aura_delay( timespan_t::from_seconds( 0.3 ) ),
  default_aura_delay_stddev( timespan_t::from_seconds( 0.05 ) ),
  cooldown_list( 0 ),
  elapsed_cpu( timespan_t::zero() ), iteration_dmg( 0 ), iteration_heal( 0 ),
  raid_dps(), total_dmg(), raid_hps(), total_heal(), simulation_length( false ),
  report_progress( 1 ),
  bloodlust_percent( 25 ), bloodlust_time( timespan_t::from_seconds( -60 ) ),
  path_str( "." ), output_file( stdout ),
  debug_exp( 0 ),
  // Report
  report_precision( 4 ),report_pets_separately( 0 ), report_targets( 1 ), report_details( 1 ),
  report_rng( 0 ), hosted_html( 0 ), print_styles( false ), report_overheal( 0 ),
  save_raid_summary( 0 ), statistics_level( 1 ), separate_stats_by_actions( 0 ),
  report_information( report_information_t() ),
  // Multi-Threading
  threads( 0 ), thread_index( index ),
  spell_query( 0 ), spell_query_level( MAX_LEVEL )
{
#if SC_DEATH_KNIGHT == 1
  register_death_knight_targetdata( this );
#endif
#if SC_DRUID == 1
  register_druid_targetdata( this );
#endif
#if SC_HUNTER == 1
  register_hunter_targetdata( this );
#endif
#if SC_MAGE == 1
  register_mage_targetdata( this );
#endif
#if SC_MONK == 1
  register_monk_targetdata( this );
#endif
#if SC_PALADIN == 1
  register_paladin_targetdata( this );
#endif
#if SC_PRIEST == 1
  register_priest_targetdata( this );
#endif
#if SC_ROGUE == 1
  register_rogue_targetdata( this );
#endif
#if SC_SHAMAN == 1
  register_shaman_targetdata( this );
#endif
#if SC_WARLOCK == 1
  register_warlock_targetdata( this );
#endif
#if SC_WARRIOR == 1
  register_warrior_targetdata( this );
#endif

  path_str += "|profiles";
  path_str += "|profiles_heal";
  path_str += "|profiles";
  path_str += DIRECTORY_DELIMITER;
  path_str += "Tier14H";
  path_str += "|profiles";
  path_str += DIRECTORY_DELIMITER;
  path_str += "Tier14N";
  path_str += "|profiles";
  path_str += DIRECTORY_DELIMITER;
  path_str += "Tier13H";
  path_str += "|profiles";
  path_str += DIRECTORY_DELIMITER;
  path_str += "Tier13N";
  path_str += "|profiles";
  path_str += DIRECTORY_DELIMITER;
  path_str += "mop_test";

  path_str += "|..";
  path_str += DIRECTORY_DELIMITER;
  path_str += "profiles";
  path_str += "|..";
  path_str += DIRECTORY_DELIMITER;
  path_str += "profiles_heal";
  path_str += "|..";
  path_str += DIRECTORY_DELIMITER;
  path_str += "profiles";
  path_str += DIRECTORY_DELIMITER;
  path_str += "mop_test";
  path_str += "|..";
  path_str += DIRECTORY_DELIMITER;
  path_str += "profiles";
  path_str += DIRECTORY_DELIMITER;
  path_str += "Tier14H";
  path_str += "|..";
  path_str += DIRECTORY_DELIMITER;
  path_str += "profiles";
  path_str += DIRECTORY_DELIMITER;
  path_str += "Tier14N";
  path_str += "|..";
  path_str += DIRECTORY_DELIMITER;
  path_str += "profiles";
  path_str += DIRECTORY_DELIMITER;
  path_str += "Tier13H";
  path_str += "|..";
  path_str += DIRECTORY_DELIMITER;
  path_str += "profiles";
  path_str += DIRECTORY_DELIMITER;
  path_str += "Tier13N";

  // Initialize the default item database source order
  static const char* const dbsources[] = { "local", "bcpapi", "wowhead", "mmoc", "armory", "ptrhead" };
  item_db_sources.assign( range::begin( dbsources ), range::end( dbsources ) );

  scaling = new scaling_t( this );
  plot    = new    plot_t( this );
  reforge_plot = new reforge_plot_t( this );

  use_optimal_buffs_and_debuffs( 1 );

  create_options();

  if ( parent )
  {
    // Import the config file
    parse_options( parent -> argc, parent -> argv );

    // Inherit 'scaling' settings from parent because these are set outside of the config file
    scaling -> scale_stat  = parent -> scaling -> scale_stat;
    scaling -> scale_value = parent -> scaling -> scale_value;

    // Inherit reporting directives from parent
    report_progress = parent -> report_progress;
    output_file     = parent -> output_file;

    // Inherit 'plot' settings from parent because are set outside of the config file
    enchant = parent -> enchant;

    seed = parent -> seed;
  }
}

// sim_t::~sim_t ============================================================

sim_t::~sim_t()
{
  flush_events();

  while ( player_t* t = target_list )
  {
    target_list = t -> next;
    delete t;
  }

  while ( player_t* p = player_list )
  {
    player_list = p -> next;
    delete p;
  }

  while ( rng_t* r = rng_list )
  {
    rng_list = r -> next;
    delete r;
  }

  range::dispose( buff_list );

  while ( cooldown_t* d = cooldown_list )
  {
    cooldown_list = d -> next;
    delete d;
  }

  delete rng;
  delete _deterministic_rng;
  delete scaling;
  delete plot;
  delete reforge_plot;

  range::dispose( raid_events );
  range::dispose( children );

  delete[] timing_wheel;
  delete spell_query;
}

// sim_t::add_event =========================================================

void sim_t::add_event( event_t* e,
                       timespan_t delta_time )
{
  if ( delta_time < timespan_t::zero() )
    delta_time = timespan_t::zero();

  e -> time = current_time + delta_time;
  e -> id   = ++id;

  if ( unlikely( ! ( delta_time.total_seconds() <= wheel_seconds ) ) )
  {
    errorf( "sim_t::add_event assertion error! delta_time > wheel_seconds, event %s from %s.\n", e -> name, e -> player ? e -> player -> name() : "no-one" );
    assert( 0 );
  }

  if ( e -> time > last_event ) last_event = e -> time;

  uint32_t slice = ( uint32_t ) ( e -> time.total_seconds() * wheel_granularity ) & wheel_mask;

  event_t** prev = &( timing_wheel[ slice ] );

  while ( ( *prev ) && ( *prev ) -> time <= e -> time ) prev = &( ( *prev ) -> next );

  e -> next = *prev;
  *prev = e;

  events_remaining++;
  if ( events_remaining > max_events_remaining ) max_events_remaining = events_remaining;
  if ( e -> player ) e -> player -> events++;

  if ( debug )
  {
    log_t::output( this, "Add Event: %s %.2f %d", e -> name, e -> time.total_seconds(), e -> id );
    if ( e -> player ) log_t::output( this, "Actor %s has %d scheduled events", e -> player -> name(), e -> player -> events );
  }
}

// sim_t::reschedule_event ==================================================

void sim_t::reschedule_event( event_t* e )
{
  if ( debug ) log_t::output( this, "Reschedule Event: %s %d", e -> name, e -> id );

  add_event( e, ( e -> reschedule_time - current_time ) );

  e -> reschedule_time = timespan_t::zero();
}

// sim_t::next_event ========================================================

event_t* sim_t::next_event()
{
  if ( events_remaining == 0 ) return 0;

  while ( true )
  {
    event_t*& event_list = timing_wheel[ timing_slice ];

    if ( event_list )
    {
      event_t* e = event_list;
      event_list = e -> next;
      events_remaining--;
      events_processed++;
      return e;
    }

    timing_slice++;
    if ( timing_slice == wheel_size )
    {
      timing_slice = 0;
      if ( debug ) log_t::output( this, "Time Wheel turns around." );
    }
  }

  return 0;
}

// sim_t::flush_events ======================================================

void sim_t::flush_events()
{
  if ( debug ) log_t::output( this, "Flush Events" );

  for ( int i=0; i < wheel_size; i++ )
  {
    while ( event_t* e = timing_wheel[ i ] )
    {
      if ( e -> player && ! e -> canceled )
      {
        // Make sure we dont recancel events, although it should
        // not technically matter
        e -> canceled = 1;
        e -> player -> events--;
        if ( e -> player -> events < 0 )
        {
          errorf( "sim_t::flush_events assertion error! flushing event %s leaves negative event count for user %s.\n", e -> name, e -> player -> name() );
          assert( 0 );
        }
      }
      timing_wheel[ i ] = e -> next;
      delete e;
    }
  }

  events_remaining = 0;
  events_processed = 0;
  timing_slice = 0;
  id = 0;
}

// sim_t::cancel_events =====================================================

void sim_t::cancel_events( player_t* p )
{
  if ( p -> events <= 0 ) return;

  if ( debug ) log_t::output( this, "Canceling events for player %s, events to cancel %d", p -> name(), p -> events );

  int end_slice = ( uint32_t ) ( last_event.total_seconds() * wheel_granularity ) & wheel_mask;

  // Loop only partial wheel, [current_time..last_event], as that's the range where there
  // are events for actors in the sim
  if ( timing_slice <= end_slice )
  {
    for ( int i = timing_slice; i <= end_slice && p -> events > 0; i++ )
    {
      for ( event_t* e = timing_wheel[ i ]; e && p -> events > 0; e = e -> next )
      {
        if ( e -> player == p )
        {
          if ( ! e -> canceled )
            p -> events--;

          e -> canceled = 1;
        }
      }
    }
  }
  // Loop only partial wheel in two places, as the wheel has wrapped around, but simulation
  // current time is still at the tail-end, [begin_slice..wheel_size[ and [0..last_event]
  else
  {
    for ( int i = timing_slice; i < wheel_size && p -> events > 0; i++ )
    {
      for ( event_t* e = timing_wheel[ i ]; e && p -> events > 0; e = e -> next )
      {
        if ( e -> player == p )
        {
          if ( ! e -> canceled )
            p -> events--;

          e -> canceled = 1;
        }
      }
    }

    for ( int i = 0; i <= end_slice && p -> events > 0; i++ )
    {
      for ( event_t* e = timing_wheel[ i ]; e && p -> events > 0; e = e -> next )
      {
        if ( e -> player == p )
        {
          if ( ! e -> canceled )
            p -> events--;

          e -> canceled = 1;
        }
      }
    }
  }

  assert( p -> events == 0 );
}

// sim_t::combat ============================================================

void sim_t::combat( int iteration )
{
  if ( debug ) log_t::output( this, "Starting Simulator" );

  current_iteration = iteration;

  combat_begin();

  while ( event_t* e = next_event() )
  {
    current_time = e -> time;

    // Perform actor event bookkeeping first
    if ( e -> player && ! e -> canceled )
    {
      e -> player -> events--;
      if ( e -> player -> events < 0 )
      {
        errorf( "sim_t::combat assertion error! canceling event %s leaves negative event count for user %s.\n", e -> name, e -> player -> name() );
        assert( 0 );
      }
    }

    if ( fixed_time || ( target -> resources.base[ RESOURCE_HEALTH ] == 0 ) )
    {
      // The first iteration is always time-limited since we do not yet have inferred health
      if ( current_time > expected_time )
      {
        if ( debug ) log_t::output( this, "Reached expected_time=%.2f, ending simulation", expected_time.total_seconds() );
        // Set this last event as canceled, so asserts dont fire when odd things happen at the
        // tail-end of the simulation iteration
        e -> canceled = 1;
        delete e;
        break;
      }
    }
    else
    {
      if ( expected_time > timespan_t::zero() && current_time > ( expected_time * 2.0 ) )
      {
        if ( debug ) log_t::output( this, "Target proving tough to kill, ending simulation" );
        // Set this last event as canceled, so asserts dont fire when odd things happen at the
        // tail-end of the simulation iteration
        e -> canceled = 1;
        delete e;
        break;
      }

      if (  target -> resources.current[ RESOURCE_HEALTH ] / target -> resources.max[ RESOURCE_HEALTH ] <= target_death_pct / 100.0 )
      {
        if ( debug ) log_t::output( this, "Target %s has died, ending simulation", target -> name() );
        // Set this last event as canceled, so asserts dont fire when odd things happen at the
        // tail-end of the simulation iteration
        e -> canceled = 1;
        delete e;
        break;
      }
    }

    if ( unlikely( e -> canceled ) )
    {
      if ( debug ) log_t::output( this, "Canceled event: %s", e -> name );
    }
    else if ( unlikely( e -> reschedule_time > e -> time ) )
    {
      reschedule_event( e );
      continue;
    }
    else
    {
      if ( debug ) log_t::output( this, "Executing event: %s", e -> name );
      e -> execute();
    }
    delete e;
  }

  combat_end();
}

// sim_t::reset =============================================================

void sim_t::reset()
{
  if ( debug ) log_t::output( this, "Resetting Simulator" );
  expected_time = max_time * ( 1.0 + vary_combat_length * iteration_adjust() );
  id = 0;
  current_time = timespan_t::zero();
  last_event = timespan_t::zero();

  for ( size_t i = 0; i < buff_list.size(); ++i )
    buff_list[ i ] -> reset();

  for ( player_t* t = target_list; t; t = t -> next )
  {
    t -> reset();
  }
  for ( player_t* p = player_list; p; p = p -> next )
  {
    p -> reset();
  }
  raid_event_t::reset( this );
}

// sim_t::combat_begin ======================================================

void sim_t::combat_begin()
{
  if ( debug ) log_t::output( this, "Combat Begin" );

  reset();

  iteration_dmg = iteration_heal = 0;

  for ( player_t* t = target_list; t; t = t -> next )
  {
    t -> combat_begin();
  }

  for ( size_t i = 0; i < buff_list.size(); ++i )
    buff_list[ i ] -> combat_begin();

  if ( overrides.attack_haste            ) auras.attack_haste            -> override();
  if ( overrides.attack_power_multiplier ) auras.attack_power_multiplier -> override();
  if ( overrides.critical_strike         ) auras.critical_strike         -> override();
  if ( overrides.mastery                 ) auras.mastery                 -> override();
  if ( overrides.spell_haste             ) auras.spell_haste             -> override();
  if ( overrides.spell_power_multiplier  ) auras.spell_power_multiplier  -> override();
  if ( overrides.stamina                 ) auras.stamina                 -> override();
  if ( overrides.str_agi_int             ) auras.str_agi_int             -> override();

  for ( player_t* t = target_list; t; t = t -> next )
  {
    if ( overrides.slowed_casting          ) t -> debuffs.slowed_casting          -> override();
    if ( overrides.magic_vulnerability    ) t -> debuffs.magic_vulnerability    -> override();
    if ( overrides.mortal_wounds          ) t -> debuffs.mortal_wounds          -> override();
    if ( overrides.physical_vulnerability ) t -> debuffs.physical_vulnerability -> override();
    if ( overrides.weakened_armor         ) t -> debuffs.weakened_armor         -> override( 3 );
    if ( overrides.weakened_blows         ) t -> debuffs.weakened_blows         -> override();
  }

  player_t::combat_begin( this );

  raid_event_t::combat_begin( this );

  for ( player_t* p = player_list; p; p = p -> next )
  {
    p -> combat_begin();
  }
  new ( this ) regen_event_t( this );


  if ( overrides.bloodlust )
  {
    // Setup a periodic check for Bloodlust

    struct bloodlust_check_t : public event_t
    {
      bloodlust_check_t( sim_t* sim ) : event_t( sim, 0 )
      {
        name = "Bloodlust Check";
        sim -> add_event( this, timespan_t::from_seconds( 1.0 ) );
      }
      virtual void execute()
      {
        player_t* t = sim -> target;
        if ( ( sim -> bloodlust_percent  > 0                && t -> health_percentage() <  sim -> bloodlust_percent ) ||
             ( sim -> bloodlust_time     < timespan_t::zero() && t -> time_to_die()       < -sim -> bloodlust_time ) ||
             ( sim -> bloodlust_time     > timespan_t::zero() && sim -> current_time      >  sim -> bloodlust_time ) )
        {
          for ( player_t* p = sim -> player_list; p; p = p -> next )
          {
            if ( p -> sleeping || p -> buffs.exhaustion -> check() )
              continue;

            p -> buffs.bloodlust -> trigger();
          }
        }
        else
        {
          new ( sim ) bloodlust_check_t( sim );
        }
      }
    };

    new ( this ) bloodlust_check_t( this );
  }
}

// sim_t::combat_end ========================================================

void sim_t::combat_end()
{
  if ( debug ) log_t::output( this, "Combat End" );

  iteration_timeline.push_back( current_time );
  simulation_length.add( current_time.total_seconds() );

  total_events_processed += events_processed;

  for ( player_t* t = target_list; t; t = t -> next )
  {
    if ( t -> is_add() ) continue;
    t -> combat_end();
  }
  player_t::combat_end( this );

  raid_event_t::combat_end( this );

  for ( player_t* p = player_list; p; p = p -> next )
  {
    if ( p -> is_pet() ) continue;
    p -> combat_end();
  }

  for ( size_t i = 0; i < buff_list.size(); ++i )
  {
    buff_t* b = buff_list[ i ];
    b -> expire();
    b -> combat_end();
  }

  total_dmg.add( iteration_dmg );
  raid_dps.add( current_time != timespan_t::zero() ? iteration_dmg / current_time.total_seconds() : 0 );
  total_heal.add( iteration_heal );
  raid_hps.add( current_time != timespan_t::zero() ? iteration_heal / current_time.total_seconds() : 0 );

  flush_events();
}

// sim_t::init ==============================================================

bool sim_t::init()
{
  if ( seed == 0 ) seed = ( int ) time( NULL );

  if ( ! parent ) srand( seed );

  rng = rng_t::create( this, "global", RNG_MERSENNE_TWISTER );

  _deterministic_rng = rng_t::create( this, "global_deterministic", RNG_MERSENNE_TWISTER );
  _deterministic_rng -> seed( 31459 + thread_index );

  if ( scaling -> smooth_scale_factors &&
       scaling -> scale_stat != STAT_NONE )
  {
    separated_rng = true;
    average_range = true;
    deterministic_rng = true;
  }

  default_rng_ = ( deterministic_rng ? _deterministic_rng : rng );

  // Timing wheel depth defaults to about 17 minutes with a granularity of 32 buckets per second.
  // This makes wheel_size = 32K and it's fully used.
  if ( wheel_seconds     <  600 ) wheel_seconds     = 1024; // 2^10  Min of 600 to ensure no wrap-around bugs with Water Shield
  if ( wheel_granularity <=   0 ) wheel_granularity = 32; // 2^5

  wheel_size = ( uint32_t ) ( wheel_seconds * wheel_granularity );

  // Round up the wheel depth to the nearest power of 2 to enable a fast "mod" operation.
  for ( wheel_mask = 2; wheel_mask < wheel_size; wheel_mask *= 2 ) { continue; }
  wheel_size = wheel_mask;
  wheel_mask--;

  // The timing wheel represents an array of event lists: Each time slice has an event list.
  delete[] timing_wheel;
  timing_wheel= new event_t*[wheel_size];
  memset( timing_wheel,0,sizeof( event_t* )*wheel_size );


  if (   queue_lag_stddev == timespan_t::zero() )   queue_lag_stddev =   queue_lag * 0.25;
  if (     gcd_lag_stddev == timespan_t::zero() )     gcd_lag_stddev =     gcd_lag * 0.25;
  if ( channel_lag_stddev == timespan_t::zero() ) channel_lag_stddev = channel_lag * 0.25;
  if ( world_lag_stddev    < timespan_t::zero() ) world_lag_stddev   =   world_lag * 0.1;

  // MoP aura initialization

  // Attack and Ranged haste, value from Swiftblade's Cunning (id=113742) (Rogue)
  auras.attack_haste = buff_creator_t( this, "attack_haste" );
  auras.attack_haste -> current_value = dbc.spell( 113742 ) -> effectN( 1 ).percent();

  // Attack Power Multiplier, value from Trueshot Aura (id=19506) (Hunter)
  auras.attack_power_multiplier = buff_creator_t( this, "attack_power_multiplier" );
  auras.attack_power_multiplier -> current_value = dbc.spell( 19506 ) -> effectN( 1 ).percent();

  // Critical Strike, value from Trueshot Aura (id=19506) (Hunter)
  auras.critical_strike = buff_creator_t( this, "critical_strike" );
  auras.critical_strike -> current_value = dbc.spell( 19506 ) -> effectN( 3 ).percent();

  // Mastery, value from Grace of Air (id=116956) (Shaman)
  auras.mastery = buff_creator_t( this, "mastery" );
  auras.mastery -> current_value = dbc.spell( 116956 ) -> effectN( 1 ).base_value();

  // Spell Haste, value from Mind Quickening (id=49868) (Priest)
  auras.spell_haste = buff_creator_t( this, "spell_haste" );
  auras.spell_haste -> current_value = dbc.spell( 49868 ) -> effectN( 1 ).percent();

  // Spell Power Multiplier, value from Burning Wrath (id=77747) (Shaman)
  auras.spell_power_multiplier = buff_creator_t( this, "spell_power_multiplier" );
  auras.spell_power_multiplier -> current_value = dbc.spell( 77747 ) -> effectN( 1 ).percent();

  // Stamina, value from fortitude (id=79104) (Priest)
  auras.stamina = buff_creator_t( this, "stamina" );
  auras.stamina -> current_value = dbc.spell( 79104 ) -> effectN( 1 ).percent();

  // Strength, Agility, and Intellect, value from Blessing of Kings (id=79062) (Paladin)
  auras.str_agi_int = buff_creator_t( this, "str_agi_int" );
  auras.str_agi_int -> current_value = dbc.spell( 79062 ) -> effectN( 1 ).percent();

  // Find Already defined target, otherwise create a new one.
  if ( debug )
    log_t::output( this, "Creating Enemies." );

  if ( target_list )
  {
    target = target_list;
  }
  else if ( ! main_target_str.empty() )
  {
    player_t* p = find_player( main_target_str );
    if ( p )
      target = p;
  }
  else
    target = player_t::create( this, "enemy", "Fluffy_Pillow" );


  if ( max_player_level < 0 )
  {
    for ( player_t* p = player_list; p; p = p -> next )
    {
      if ( p -> is_enemy() || p -> is_add() )
        continue;
      if ( max_player_level < p -> level )
        max_player_level = p -> level;
    }
  }

  if ( ! player_t::init( this ) ) return false;

  // Target overrides 2
  for ( player_t* t = target_list; t; t = t -> next )
  {
    if ( ! target_race.empty() )
    {
      t -> race = util_t::parse_race_type( target_race );
      t -> race_str = util_t::race_type_string( t -> race );
    }
  }

  raid_event_t::init( this );

  if ( report_precision < 0 ) report_precision = 4;

  raid_dps.reserve( iterations );
  total_dmg.reserve( iterations );
  raid_hps.reserve( iterations );
  total_heal.reserve( iterations );
  simulation_length.reserve( iterations );

  return canceled ? false : true;
}

// compare_dps ==============================================================

struct compare_dps
{
  bool operator()( player_t* l, player_t* r ) const
  {
    return l -> dps.mean > r -> dps.mean;
  }
};

// compare_hps ==============================================================

struct compare_hps
{
  bool operator()( player_t* l, player_t* r ) const
  {
    return l -> hps.mean > r -> hps.mean;
  }
};

// compare_name =============================================================

struct compare_name
{
  bool operator()( player_t* l, player_t* r ) const
  {
    if ( l -> type != r -> type )
    {
      return l -> type < r -> type;
    }
    if ( l -> primary_tree() != r -> primary_tree() )
    {
      return l -> primary_tree() < r -> primary_tree();
    }
    return l -> name_str < r -> name_str;
  }
};

// compare_name =============================================================

struct compare_stats_name
{
  bool operator()( stats_t* l, stats_t* r ) const
  {
    return l -> name_str <= r -> name_str;
  }
};

namespace {

void player_convergence( const int& iterations, const int& convergence_scale, const double& confidence_estimator,
                         const sample_data_t& dps, std::vector<double>& dps_convergence_error, const double& dps_error, double& dps_convergence )
{
  // Error Convergence ======================================================

  int    convergence_iterations = 0;
  double convergence_dps = 0;
  double convergence_min = +1.0E+50;
  double convergence_max = -1.0E+50;
  double convergence_std_dev = 0;

  if ( iterations > 1 && convergence_scale > 1 && !dps.simple )
  {
    for ( int i=0; i < iterations; i += convergence_scale )
    {
      double i_dps = dps.data()[ i ];
      convergence_dps += i_dps;
      if ( convergence_min > i_dps ) convergence_min = i_dps;
      if ( convergence_max < i_dps ) convergence_max = i_dps;
    }
    convergence_iterations = ( iterations + convergence_scale - 1 ) / convergence_scale;
    convergence_dps /= convergence_iterations;

    assert( dps_convergence_error.empty() );
    dps_convergence_error.reserve( iterations );

    double sum_of_squares = 0;

    for ( int i=0; i < iterations; i++ )
    {
      dps_convergence_error.push_back( confidence_estimator * sqrt( sum_of_squares / i ) / sqrt( ( float ) i ) );

      double delta = dps.data()[ i ] - convergence_dps;
      double delta_squared = delta * delta;

      sum_of_squares += delta_squared;

      if ( ( i % convergence_scale ) == 0 )
        convergence_std_dev += delta_squared;
    }
  }

  if ( convergence_iterations > 1 ) convergence_std_dev /= convergence_iterations;
  convergence_std_dev = sqrt( convergence_std_dev );
  double convergence_error = confidence_estimator * convergence_std_dev;
  if ( convergence_iterations > 1 ) convergence_error /= sqrt( ( float ) convergence_iterations );

  if ( convergence_error > 0 )
    dps_convergence = convergence_error / ( dps_error * convergence_scale );
}
}
// sim_t::analyze_player ====================================================

void sim_t::analyze_player( player_t* p )
{
  assert( iterations > 0 );

  p -> pre_analyze_hook();

  // Sample Data Analysis ========================================================

  // sample_data_t::analyze(calc_basics,calc_variance,sort )

  p -> deaths.analyze( true, true, true, 50 );

  p -> fight_length.analyze( true, true );
  p -> waiting_time.analyze();
  p -> executed_foreground_actions.analyze();

  p -> dmg.analyze( true, true );
  p -> compound_dmg.analyze();
  p -> dps.analyze( true, true, true, 50 );
  p -> dpse.analyze();

  p -> dmg_taken.analyze();
  p -> dtps.analyze( true, true );

  p -> heal.analyze();
  p -> compound_heal.analyze();
  p -> hps.analyze( true, true, true, 50 );
  p -> hpse.analyze();

  p -> heal_taken.analyze();
  p -> htps.analyze( true, true );

  p -> deaths_error = p -> deaths.mean_std_dev * confidence_estimator;
  p -> dps_error = p -> dps.mean_std_dev * confidence_estimator;
  p -> dtps_error = p -> dtps.mean_std_dev * confidence_estimator;
  p -> hps_error = p -> hps.mean_std_dev * confidence_estimator;

  for ( size_t i = 0; i < p -> buff_list.size(); ++i )
    p -> buff_list[ i ] -> analyze();

  for ( benefit_t* u = p -> benefit_list; u; u = u -> next )
    u -> analyze();

  for ( uptime_t* u = p -> uptime_list; u; u = u -> next )
    u -> analyze();

  range::sort( p -> stats_list, compare_stats_name() );

  if ( p -> quiet ) return;
  if ( p -> fight_length.mean == 0 ) return;

  // Pet Chart Adjustment ===================================================
  size_t max_buckets = static_cast<size_t>( p -> fight_length.max );

  // Make the pet graphs the same length as owner's
  if ( p -> is_pet() )
  {
    player_t* o = p -> cast_pet() -> owner;
    max_buckets = static_cast<size_t>( o -> fight_length.max );
  }

  // Stats Analysis =========================================================
  std::vector<stats_t*> stats_list;

  for ( size_t i = 0; i < p -> stats_list.size(); ++i )
    stats_list.push_back( p -> stats_list[ i ] );

  for ( size_t i = 0; i < p -> pet_list.size(); ++i )
  {
    pet_t* pet = p -> pet_list[ i ];
    for ( size_t i = 0; i < pet -> stats_list.size(); ++i )
      stats_list.push_back( pet -> stats_list[ i ] );
  }

  size_t num_stats = stats_list.size();

  if ( ! p -> is_pet() )
  {
    for ( size_t i = 0; i < num_stats; i++ )
    {
      stats_t* s = stats_list[ i ];
      s -> analyze();

      if ( s -> type == STATS_DMG )
        s -> portion_amount = p -> compound_dmg.mean ? s -> compound_amount / p -> compound_dmg.mean : 0 ;
      else
        s -> portion_amount = p -> compound_heal.mean ? s -> compound_amount / p -> compound_heal.mean : 0;
    }
  }

  // Actor Lists ============================================================
  if (  ! p -> quiet && ! p -> is_enemy() && ! p -> is_add() && ! ( p -> is_pet() && report_pets_separately ) )
  {
    players_by_dps.push_back( p );
    players_by_hps.push_back( p );
    players_by_name.push_back( p );
  }
  if ( ! p -> quiet && ( p -> is_enemy() || p -> is_add() ) && ! ( p -> is_pet() && report_pets_separately ) )
    targets_by_name.push_back( p );


  // Resources & Gains ======================================================
  for ( size_t i = 0; i < p -> resource_timeline_count; ++i )
  {
    std::vector<double>& timeline = p -> resource_timelines[ i ].timeline;
    if ( timeline.size() > max_buckets ) timeline.resize( max_buckets );

    assert( timeline.size() == max_buckets );
    for ( size_t j = 0; j < max_buckets; j++ )
      timeline[ j ] /= divisor_timeline[ j ] * regen_periodicity.total_seconds();
  }

  for ( resource_type_e i = RESOURCE_NONE; i < RESOURCE_MAX; ++i )
  {
    p -> resource_lost  [ i ] /= iterations;
    p -> resource_gained[ i ] /= iterations;
  }

  double rl = p -> resource_lost[ p -> primary_resource() ];
  p -> dpr = ( rl > 0 ) ? ( p -> dmg.mean / rl ) : -1.0;
  p -> hpr = ( rl > 0 ) ? ( p -> heal.mean / rl ) : -1.0;

  p -> rps_loss = p -> resource_lost  [ p -> primary_resource() ] / p -> fight_length.mean;
  p -> rps_gain = p -> resource_gained[ p -> primary_resource() ] / p -> fight_length.mean;

  for ( gain_t* g = p -> gain_list; g; g = g -> next )
    g -> analyze( this );

  for ( size_t i = 0; i < p -> pet_list.size(); ++i )
  {
    pet_t* pet = p -> pet_list[ i ];
    for ( gain_t* g = pet -> gain_list; g; g = g -> next )
      g -> analyze( this );
  }

  // Procs ==================================================================

  for ( proc_t* proc = p -> proc_list; proc; proc = proc -> next )
    proc -> analyze();

  // Damage Timelines =======================================================

  p -> timeline_dmg.assign( max_buckets, 0 );
  for ( size_t i = 0, is_hps = ( p -> primary_role() == ROLE_HEAL ); i < num_stats; i++ )
  {
    stats_t* s = stats_list[ i ];
    if ( ( s -> type != STATS_DMG ) == is_hps )
    {
      size_t j_max = std::min( max_buckets, s -> timeline_amount.size() );
      for ( size_t j = 0; j < j_max; j++ )
        p -> timeline_dmg[ j ] += s -> timeline_amount[ j ];
    }
  }

  p -> timeline_dps.reserve( max_buckets );
  range::sliding_window_average<10>( p -> timeline_dmg,
                                     std::back_inserter( p -> timeline_dps ) );
  assert( p -> timeline_dps.size() == ( std::size_t ) max_buckets );

  // Error Convergence ======================================================
  player_convergence( iterations, convergence_scale, confidence_estimator,
                      p -> dps, p -> dps_convergence_error, p -> dps_error, p -> dps_convergence );

}

// sim_t::analyze ===========================================================

void sim_t::analyze()
{
  simulation_length.analyze( true, true, true, 50 );
  if ( simulation_length.mean == 0 ) return;

  // divisor_timeline is necessary because not all iterations go the same length of time
  int max_buckets = ( int ) simulation_length.max + 1;
  divisor_timeline.assign( max_buckets, 0 );

  size_t num_timelines = iteration_timeline.size();
  for ( size_t i=0; i < num_timelines; i++ )
  {
    int last = ( int ) floor( iteration_timeline[ i ].total_seconds() );
    size_t num_buckets = divisor_timeline.size();
    if ( 1 + last > ( int ) num_buckets ) divisor_timeline.resize( 1 + last, 0 );
    for ( int j=0; j <= last; j++ ) divisor_timeline[ j ] += 1;
  }

  for ( size_t i = 0; i < buff_list.size(); ++i )
    buff_list[ i ] -> analyze();

  total_dmg.analyze();
  raid_dps.analyze();
  total_heal.analyze();
  raid_hps.analyze();

  confidence_estimator = rng -> stdnormal_inv( 1.0 - ( 1.0 - confidence ) / 2.0 );

  for ( unsigned int i = 0; i < actor_list.size(); i++ )
    analyze_player( actor_list[i] );

  range::sort( players_by_dps, compare_dps() );
  range::sort( players_by_hps, compare_hps() );
  range::sort( players_by_name, compare_name() );
  range::sort( targets_by_name, compare_name() );
}

// sim_t::iterate ===========================================================

bool sim_t::iterate()
{
  if ( ! init() ) return false;

  int message_interval = iterations/10;
  int message_index = 10;

  for ( int i=0; i < iterations; i++ )
  {
    if ( canceled )
    {
      iterations = current_iteration + 1;
      break;
    }

    if ( report_progress && ( message_interval > 0 ) && ( i % message_interval == 0 ) && ( message_index > 0 ) )
    {
      util_t::fprintf( stdout, "%d... ", message_index-- );
      fflush( stdout );
    }
    combat( i );
  }
  if ( report_progress ) util_t::fprintf( stdout, "\n" );

  reset();

  return true;
}

// sim_t::merge =============================================================

void sim_t::merge( sim_t& other_sim )
{
  iterations             += other_sim.iterations;
  total_events_processed += other_sim.total_events_processed;

  simulation_length.merge( other_sim.simulation_length );
  total_dmg.merge( other_sim.total_dmg );
  raid_dps.merge( other_sim.raid_dps );
  total_heal.merge( other_sim.total_heal );
  raid_hps.merge( other_sim.raid_hps );

  if ( max_events_remaining < other_sim.max_events_remaining ) max_events_remaining = other_sim.max_events_remaining;

  range::copy( other_sim.iteration_timeline, std::back_inserter( iteration_timeline ) );

  for ( size_t i = 0; i < buff_list.size(); ++i )
  {
    buff_list[ i ] -> merge( buff_t::find( &other_sim, buff_list[ i ] -> name_str.c_str() ) );
  }

  for ( unsigned int i = 0; i < actor_list.size(); i++ )
  {
    player_t* p = actor_list[i];
    player_t* other_p = other_sim.find_player( p -> index );
    assert( other_p );
    p -> merge( *other_p );
  }
}

// sim_t::merge =============================================================

void sim_t::merge()
{
  int num_children = ( int ) children.size();

  for ( int i=0; i < num_children; i++ )
  {
    sim_t* child = children[ i ];
    child -> wait();
    merge( *child );
    delete child;
  }

  children.clear();
}

// sim_t::partition =========================================================

void sim_t::partition()
{
  if ( threads <= 1 ) return;
  if ( iterations < threads ) return;

#if defined( NO_THREADS )
  util_t::fprintf( output_file, "simulationcraft: This executable was built without thread support, please remove 'threads=N' from config file.\n" );
  exit( 0 );
#endif

  iterations /= threads;

  int num_children = threads - 1;
  children.resize( num_children );

  for ( int i=0; i < num_children; i++ )
  {
    sim_t* child = children[ i ] = new sim_t( this, i+1 );
    child -> iterations /= threads;
    child -> report_progress = 0;
  }

  for ( int i=0; i < num_children; i++ )
    children[ i ] -> launch();
}

// sim_t::execute ===========================================================

bool sim_t::execute()
{
  int64_t start_time = util_t::milliseconds();

  partition();
  if ( ! iterate() ) return false;
  merge();
  analyze();

  elapsed_cpu = timespan_t::from_millis( ( util_t::milliseconds() - start_time ) );

  return true;
}

// sim_t::find_player =======================================================

player_t* sim_t::find_player( const std::string& name ) const
{
  for ( size_t i = 0; i < actor_list.size(); i++ )
  {
    player_t* p = actor_list[ i ];
    if ( name == p -> name() ) return p;
  }
  return 0;
}

// sim_t::find_player =======================================================

player_t* sim_t::find_player( int index ) const
{
  for ( size_t i = 0; i < actor_list.size(); i++ )
  {
    player_t* p = actor_list[ i ];
    if ( index == p -> index ) return p;
  }
  return 0;
}

// sim_t::get_cooldown ======================================================

cooldown_t* sim_t::get_cooldown( const std::string& name )
{
  cooldown_t* c=0;

  for ( c = cooldown_list; c; c = c -> next )
  {
    if ( c -> name_str == name )
      return c;
  }

  c = new cooldown_t( name, this );

  cooldown_t** tail = &cooldown_list;

  while ( *tail && name > ( ( *tail ) -> name_str ) )
  {
    tail = &( ( *tail ) -> next );
  }

  c -> next = *tail;
  *tail = c;

  return c;
}

// sim_t::use_optimal_buffs_and_debuffs =====================================

void sim_t::use_optimal_buffs_and_debuffs( int value )
{
  optimal_raid = value;

  overrides.attack_haste           = optimal_raid;
  overrides.attack_power_multiplier= optimal_raid;
  overrides.critical_strike        = optimal_raid;
  overrides.mastery                = optimal_raid;
  overrides.spell_haste            = optimal_raid;
  overrides.spell_power_multiplier = optimal_raid;
  overrides.stamina                = optimal_raid;
  overrides.str_agi_int            = optimal_raid;

  overrides.slowed_casting          = optimal_raid;
  overrides.magic_vulnerability    = optimal_raid;
  overrides.mortal_wounds          = optimal_raid;
  overrides.physical_vulnerability = optimal_raid;
  overrides.weakened_armor         = optimal_raid;
  overrides.weakened_blows         = optimal_raid;

  overrides.bloodlust              = optimal_raid;
  overrides.honor_among_thieves    = optimal_raid;
}

// sim_t::time_to_think =====================================================

bool sim_t::time_to_think( timespan_t proc_time )
{
  if ( proc_time == timespan_t::zero() ) return false;
  if ( proc_time < timespan_t::zero() ) return true;
  return current_time - proc_time > reaction_time;
}

// sim_t::range =============================================================

double sim_t::range( double min,
                     double max )
{
  if ( average_range ) return ( min + max ) / 2.0;

  return default_rng_ -> range( min, max );
}

// sim_t::gauss =============================================================

double sim_t::gauss( double mean,
                     double stddev )
{
  if ( average_gauss ) return mean;

  return default_rng_ -> gauss( mean, stddev );
}

// sim_t::gauss =============================================================

timespan_t sim_t::gauss( timespan_t mean,
                         timespan_t stddev )
{
  return timespan_t::from_native( gauss( timespan_t::to_native( mean ), timespan_t::to_native( stddev ) ) );
}

// sim_t::get_rng ===========================================================

rng_t* sim_t::get_rng( const std::string& n, int type )
{
  assert( rng );

  if ( type == RNG_GLOBAL ) return rng;
  if ( type == RNG_DETERMINISTIC ) return _deterministic_rng;

  if ( ! separated_rng ) return default_rng_;

  rng_t* r=0;

  for ( r = rng_list; r; r = r -> next )
  {
    if ( r -> name_str == n )
      return r;
  }

  r = rng_t::create( this, n, static_cast<rng_type_e> ( type ) );
  r -> next = rng_list;
  rng_list = r;

  return r;
}

// sim_t::iteration_adjust ==================================================

double sim_t::iteration_adjust()
{
  if ( iterations <= 1 )
    return 0.0;

  if ( current_iteration == 0 )
    return 0.0;

  return ( 2.0 * current_iteration / ( double ) iterations ) - 1.0;
}

// sim_t::create_expression =================================================

action_expr_t* sim_t::create_expression( action_t* a,
                                         const std::string& name_str )
{
  if ( name_str == "time" )
  {
    struct time_expr_t : public action_expr_t
    {
      time_expr_t( action_t* a ) : action_expr_t( a, "time", TOK_NUM ) {}
      virtual int evaluate() { result_num = action -> sim -> current_time.total_seconds();  return TOK_NUM; }
    };
    return new time_expr_t( a );
  }

  if ( util_t::str_compare_ci( name_str, "enemies" ) )
  {
    struct enemy_amount_expr_t : public action_expr_t
    {
      enemy_amount_expr_t( action_t* a ) : action_expr_t( a, "enemy_amount", TOK_NUM ) { }
      virtual int evaluate() { result_num = action -> sim -> num_enemies; return TOK_NUM; }
    };
    return new enemy_amount_expr_t( a );
  }

  std::vector<std::string> splits;
  int num_splits = util_t::string_split( splits, name_str, "." );

  if ( num_splits == 3 )
  {
    if ( splits[ 0 ] == "aura" )
    {
      buff_t* buff = buff_t::find( this, splits[ 1 ] );
      if ( ! buff ) return 0;
      return buff -> create_expression( a, splits[ 2 ] );
    }
  }
  if ( num_splits >= 3 && splits[ 0 ] == "actors" )
  {
    player_t* actor = sim_t::find_player( splits[ 1 ] );
    if ( ! target ) return 0;
    std::string rest = splits[2];
    for ( int i = 3; i < num_splits; ++i )
      rest += '.' + splits[i];
    return actor -> create_expression( a, rest );
  }
  if ( num_splits >= 2 && splits[ 0 ] == "target" )
  {
    std::string rest = splits[1];
    for ( int i = 2; i < num_splits; ++i )
      rest += '.' + splits[i];
    return target -> create_expression( a, rest );
  }

  return 0;
}

// sim_t::print_options =====================================================

void sim_t::print_options()
{
  util_t::fprintf( output_file, "\nWorld of Warcraft Raid Simulator Options:\n" );

  int num_options = ( int ) options.size();

  util_t::fprintf( output_file, "\nSimulation Engine:\n" );
  for ( int i=0; i < num_options; i++ ) options[ i ].print( output_file );

  for ( player_t* p = player_list; p; p = p -> next )
  {
    num_options = ( int ) p -> options.size();

    util_t::fprintf( output_file, "\nPlayer: %s (%s)\n", p -> name(), util_t::player_type_string( p -> type ) );
    for ( int i=0; i < num_options; i++ ) p -> options[ i ].print( output_file );
  }

  util_t::fprintf( output_file, "\n" );
  fflush( output_file );
}

// sim_t::create_options ====================================================

void sim_t::create_options()
{
  option_t global_options[] =
  {
    // General
    { "iterations",                       OPT_INT,    &( iterations                               ) },
    { "max_time",                         OPT_TIMESPAN, &( max_time                               ) },
    { "fixed_time",                       OPT_BOOL,   &( fixed_time                               ) },
    { "vary_combat_length",               OPT_FLT,    &( vary_combat_length                       ) },
    { "optimal_raid",                     OPT_FUNC,   ( void* ) ::parse_optimal_raid                },
    { "ptr",                              OPT_FUNC,   ( void* ) ::parse_ptr                         },
    { "threads",                          OPT_INT,    &( threads                                  ) },
    { "confidence",                       OPT_FLT,    &( confidence                               ) },
    { "spell_query",                      OPT_FUNC,   ( void* ) ::parse_spell_query                 },
    { "item_db_source",                   OPT_FUNC,   ( void* ) ::parse_item_sources                },
    { "proxy",                            OPT_FUNC,   ( void* ) ::parse_proxy                       },
    // Lag
    { "channel_lag",                      OPT_TIMESPAN, &( channel_lag                            ) },
    { "channel_lag_stddev",               OPT_TIMESPAN, &( channel_lag_stddev                     ) },
    { "gcd_lag",                          OPT_TIMESPAN, &( gcd_lag                                ) },
    { "gcd_lag_stddev",                   OPT_TIMESPAN, &( gcd_lag_stddev                         ) },
    { "queue_lag",                        OPT_TIMESPAN, &( queue_lag                              ) },
    { "queue_lag_stddev",                 OPT_TIMESPAN, &( queue_lag_stddev                       ) },
    { "queue_gcd_reduction",              OPT_TIMESPAN, &( queue_gcd_reduction                    ) },
    { "strict_gcd_queue",                 OPT_BOOL,   &( strict_gcd_queue                         ) },
    { "default_world_lag",                OPT_TIMESPAN, &( world_lag                              ) },
    { "default_world_lag_stddev",         OPT_TIMESPAN, &( world_lag_stddev                       ) },
    { "default_aura_delay",               OPT_TIMESPAN, &( default_aura_delay                     ) },
    { "default_aura_delay_stddev",        OPT_TIMESPAN, &( default_aura_delay_stddev              ) },
    { "default_skill",                    OPT_FLT,    &( default_skill                            ) },
    { "reaction_time",                    OPT_TIMESPAN, &( reaction_time                          ) },
    { "travel_variance",                  OPT_FLT,    &( travel_variance                          ) },
    // Output
    { "save_profiles",                    OPT_BOOL,   &( save_profiles                            ) },
    { "default_actions",                  OPT_BOOL,   &( default_actions                          ) },
    { "debug",                            OPT_BOOL,   &( debug                                    ) },
    { "html",                             OPT_STRING, &( html_file_str                            ) },
    { "hosted_html",                      OPT_BOOL,   &( hosted_html                              ) },
    { "print_styles",                     OPT_BOOL,   &( print_styles                             ) },
    { "xml",                              OPT_STRING, &( xml_file_str                             ) },
    { "xml_style",                        OPT_STRING, &( xml_stylesheet_file_str                  ) },
    { "log",                              OPT_BOOL,   &( log                                      ) },
    { "output",                           OPT_STRING, &( output_file_str                          ) },
    { "path",                             OPT_STRING, &( path_str                                 ) },
    { "path+",                            OPT_APPEND, &( path_str                                 ) },
    { "save_raid_summary",                OPT_BOOL,   &( save_raid_summary                        ) },
    // Bloodlust
    { "bloodlust_percent",                OPT_INT,    &( bloodlust_percent                        ) },
    { "bloodlust_time",                   OPT_INT,    &( bloodlust_time                           ) },
    // Overrides"
    { "override.bloodlust",               OPT_BOOL,   &( overrides.bloodlust                      ) },
    { "override.honor_among_thieves",     OPT_BOOL,   &( overrides.honor_among_thieves            ) },
    // Regen
    { "regen_periodicity",                OPT_TIMESPAN, &( regen_periodicity                      ) },
    // RNG
    { "separated_rng",                    OPT_BOOL,   &( separated_rng                            ) },
    { "deterministic_rng",                OPT_BOOL,   &( deterministic_rng                        ) },
    { "average_range",                    OPT_BOOL,   &( average_range                            ) },
    { "average_gauss",                    OPT_BOOL,   &( average_gauss                            ) },
    { "convergence_scale",                OPT_INT,    &( convergence_scale                        ) },
    // Misc
    { "party",                            OPT_LIST,   &( party_encoding                           ) },
    { "active",                           OPT_FUNC,   ( void* ) ::parse_active                      },
    { "armor_update_internval",           OPT_INT,    &( armor_update_interval                    ) },
    { "aura_delay",                       OPT_TIMESPAN, &( aura_delay                             ) },
    { "seed",                             OPT_INT,    &( seed                                     ) },
    { "wheel_granularity",                OPT_FLT,    &( wheel_granularity                        ) },
    { "wheel_seconds",                    OPT_INT,    &( wheel_seconds                            ) },
    { "reference_player",                 OPT_STRING, &( reference_player_str                     ) },
    { "raid_events",                      OPT_STRING, &( raid_events_str                          ) },
    { "raid_events+",                     OPT_APPEND, &( raid_events_str                          ) },
    { "fight_style",                      OPT_FUNC,   ( void* ) ::parse_fight_style                 },
    { "debug_exp",                        OPT_INT,    &( debug_exp                                ) },
    { "weapon_speed_scale_factors",       OPT_BOOL,   &( weapon_speed_scale_factors               ) },
    { "main_target",                      OPT_STRING, &( main_target_str                          ) },
    { "default_dtr_proc_chance",          OPT_FLT,    &( dtr_proc_chance                          ) },
    { "target_death_pct",                 OPT_FLT,    &( target_death_pct                         ) },
    { "target_level",                     OPT_INT,    &( target_level                             ) },
    { "target_race",                      OPT_STRING, &( target_race                              ) },
    // Character Creation
    { "death_knight",                     OPT_FUNC,   ( void* ) ::parse_player                      },
    { "deathknight",                      OPT_FUNC,   ( void* ) ::parse_player                      },
    { "druid",                            OPT_FUNC,   ( void* ) ::parse_player                      },
    { "hunter",                           OPT_FUNC,   ( void* ) ::parse_player                      },
    { "mage",                             OPT_FUNC,   ( void* ) ::parse_player                      },
    { "monk",                             OPT_FUNC,   ( void* ) ::parse_player                      },
    { "priest",                           OPT_FUNC,   ( void* ) ::parse_player                      },
    { "paladin",                          OPT_FUNC,   ( void* ) ::parse_player                      },
    { "rogue",                            OPT_FUNC,   ( void* ) ::parse_player                      },
    { "shaman",                           OPT_FUNC,   ( void* ) ::parse_player                      },
    { "warlock",                          OPT_FUNC,   ( void* ) ::parse_player                      },
    { "warrior",                          OPT_FUNC,   ( void* ) ::parse_player                      },
    { "enemy",                            OPT_FUNC,   ( void* ) ::parse_player                      },
    { "pet",                              OPT_FUNC,   ( void* ) ::parse_player                      },
    { "copy",                             OPT_FUNC,   ( void* ) ::parse_player                      },
    { "armory",                           OPT_FUNC,   ( void* ) ::parse_armory                      },
    { "guild",                            OPT_FUNC,   ( void* ) ::parse_guild                       },
    { "wowhead",                          OPT_FUNC,   ( void* ) ::parse_armory                      },
    { "chardev",                          OPT_FUNC,   ( void* ) ::parse_armory                      },
    { "rawr",                             OPT_FUNC,   ( void* ) ::parse_rawr                        },
    { "wowreforge",                       OPT_FUNC,   ( void* ) ::parse_armory                      },
    { "http_clear_cache",                 OPT_FUNC,   ( void* ) ::http_t::clear_cache               },
    { "cache_items",                      OPT_FUNC,   ( void* ) ::parse_cache                       },
    { "cache_players",                    OPT_FUNC,   ( void* ) ::parse_cache                       },
    { "default_region",                   OPT_STRING, &( default_region_str                       ) },
    { "default_server",                   OPT_STRING, &( default_server_str                       ) },
    { "save_prefix",                      OPT_STRING, &( save_prefix_str                          ) },
    { "save_suffix",                      OPT_STRING, &( save_suffix_str                          ) },
    { "save_talent_str",                  OPT_BOOL,   &( save_talent_str                          ) },
    // Stat Enchants
    { "default_enchant_strength",                 OPT_FLT,  &( enchant.attribute[ ATTR_STRENGTH  ] ) },
    { "default_enchant_agility",                  OPT_FLT,  &( enchant.attribute[ ATTR_AGILITY   ] ) },
    { "default_enchant_stamina",                  OPT_FLT,  &( enchant.attribute[ ATTR_STAMINA   ] ) },
    { "default_enchant_intellect",                OPT_FLT,  &( enchant.attribute[ ATTR_INTELLECT ] ) },
    { "default_enchant_spirit",                   OPT_FLT,  &( enchant.attribute[ ATTR_SPIRIT    ] ) },
    { "default_enchant_spell_power",              OPT_FLT,  &( enchant.spell_power                 ) },
    { "default_enchant_mp5",                      OPT_FLT,  &( enchant.mp5                         ) },
    { "default_enchant_attack_power",             OPT_FLT,  &( enchant.attack_power                ) },
    { "default_enchant_expertise_rating",         OPT_FLT,  &( enchant.expertise_rating            ) },
    { "default_enchant_armor",                    OPT_FLT,  &( enchant.bonus_armor                 ) },
    { "default_enchant_dodge_rating",             OPT_FLT,  &( enchant.dodge_rating                ) },
    { "default_enchant_parry_rating",             OPT_FLT,  &( enchant.parry_rating                ) },
    { "default_enchant_block_rating",             OPT_FLT,  &( enchant.block_rating                ) },
    { "default_enchant_haste_rating",             OPT_FLT,  &( enchant.haste_rating                ) },
    { "default_enchant_mastery_rating",           OPT_FLT,  &( enchant.mastery_rating              ) },
    { "default_enchant_hit_rating",               OPT_FLT,  &( enchant.hit_rating                  ) },
    { "default_enchant_crit_rating",              OPT_FLT,  &( enchant.crit_rating                 ) },
    { "default_enchant_health",                   OPT_FLT,  &( enchant.resource[ RESOURCE_HEALTH ] ) },
    { "default_enchant_mana",                     OPT_FLT,  &( enchant.resource[ RESOURCE_MANA   ] ) },
    { "default_enchant_rage",                     OPT_FLT,  &( enchant.resource[ RESOURCE_RAGE   ] ) },
    { "default_enchant_energy",                   OPT_FLT,  &( enchant.resource[ RESOURCE_ENERGY ] ) },
    { "default_enchant_focus",                    OPT_FLT,  &( enchant.resource[ RESOURCE_FOCUS  ] ) },
    { "default_enchant_runic",                    OPT_FLT,  &( enchant.resource[ RESOURCE_RUNIC_POWER  ] ) },
    // Report
    { "report_precision",                 OPT_INT,    &( report_precision                         ) },
    { "report_pets_separately",           OPT_BOOL,   &( report_pets_separately                   ) },
    { "report_targets",                   OPT_BOOL,   &( report_targets                           ) },
    { "report_details",                   OPT_BOOL,   &( report_details                           ) },
    { "report_rng",                       OPT_BOOL,   &( report_rng                               ) },
    { "report_overheal",                  OPT_BOOL,   &( report_overheal                          ) },
    { "statistics_level",                 OPT_INT,    &( statistics_level                         ) },
    { "separate_stats_by_actions",        OPT_BOOL,    &( separate_stats_by_actions                         ) },
    { NULL, OPT_UNKNOWN, NULL }
  };

  option_t::copy( options, global_options );
}

// sim_t::parse_option ======================================================

bool sim_t::parse_option( const std::string& name,
                          const std::string& value )
{
  if ( canceled ) return false;

  if ( active_player )
    if ( option_t::parse( this, active_player -> options, name, value ) )
      return true;

  if ( option_t::parse( this, options, name, value ) )
    return true;

  return false;
}

// sim_t::parse_options =====================================================

bool sim_t::parse_options( int    _argc,
                           char** _argv )
{
  argc = _argc;
  argv = _argv;

  if ( argc <= 1 ) return false;

  if ( ! parent )
    cache::advance_era();

  for ( int i=1; i < argc; i++ )
  {
    if ( ! option_t::parse_line( this, argv[ i ] ) )
      return false;
  }

  if ( player_list == NULL && spell_query == NULL )
  {
    errorf( "Nothing to sim!\n" );
    cancel();
    return false;
  }

  if ( parent )
  {
    debug = 0;
    log = 0;
  }
  else if ( ! output_file_str.empty() )
  {
    FILE* f = fopen( output_file_str.c_str(), "w" );
    if ( f )
    {
      output_file = f;
    }
    else
    {
      errorf( "Unable to open output file '%s'\n", output_file_str.c_str() );
      cancel();
      return false;
    }
  }
  if ( debug )
  {
    log = 1;
    print_options();
  }
  if ( log )
  {
    iterations = 1;
    threads = 1;
  }

  return true;
}

// sim_t::cancel ============================================================

void sim_t::cancel()
{
  if ( canceled ) return;

  if ( current_iteration >= 0 )
  {
    errorf( "Simulation has been canceled after %d iterations! (thread=%d)\n", current_iteration+1, thread_index );
  }
  else
  {
    errorf( "Simulation has been canceled during player setup! (thread=%d)\n", thread_index );
  }
  fflush( output_file );

  canceled = 1;

  int num_children = ( int ) children.size();

  for ( int i=0; i < num_children; i++ )
  {
    children[ i ] -> cancel();
  }
}

// sim_t::progress ==========================================================

double sim_t::progress( std::string& phase )
{
  if ( canceled )
  {
    phase = "Canceled";
    return 1.0;
  }

  if ( plot -> num_plot_stats > 0 &&
       plot -> remaining_plot_stats > 0 )
  {
    return plot -> progress( phase );
  }
  else if ( scaling -> calculate_scale_factors &&
            scaling -> num_scaling_stats > 0 &&
            scaling -> remaining_scaling_stats > 0 )
  {
    return scaling -> progress( phase );
  }
  else if ( reforge_plot -> num_stat_combos > 0 )
  {
    return reforge_plot -> progress( phase );
  }
  else if ( current_iteration >= 0 )
  {
    phase = "Simulating";
    return current_iteration / ( double ) iterations;
  }
  else if ( current_slot >= 0 )
  {
    phase = current_name;
    return current_slot / ( double ) SLOT_MAX;
  }

  return 0.0;
}

// sim_t::main ==============================================================

int sim_t::main( int argc, char** argv )
{
  sim_signal_handler_t handler( this );

  http_t::cache_load();
  dbc_t::init();

  if ( ! parse_options( argc, argv ) )
  {
    errorf( "ERROR! Incorrect option format..\n" );
    cancel();
  }

  if ( canceled ) return 0;

  util_t::fprintf( output_file, "\nSimulationCraft %s-%s for World of Warcraft %s %s (build level %s)\n",
                   SC_MAJOR_VERSION, SC_MINOR_VERSION, dbc_t::wow_version( dbc.ptr ), ( dbc.ptr ? "PTR" : "Live" ), dbc_t::build_level( dbc.ptr ) );
  fflush( output_file );

  if ( spell_query )
  {
    spell_query -> evaluate();
    report_t::print_spell_query( this, spell_query_level );
  }
  else if ( need_to_save_profiles( this ) )
  {
    init();
    util_t::fprintf( stdout, "\nGenerating profiles... \n" ); fflush( stdout );
    report_t::print_profiles( this );
  }
  else
  {
    if ( max_time <= timespan_t::zero() )
    {
      util_t::fprintf( output_file, "simulationcraft: One of -max_time or -target_health must be specified.\n" );
      exit( 0 );
    }
    if ( abs( vary_combat_length ) >= 1.0 )
    {
      util_t::fprintf( output_file, "\n |vary_combat_length| >= 1.0, overriding to 0.0.\n" );
      vary_combat_length = 0.0;
    }
    if ( confidence <= 0.0 || confidence >= 1.0 )
    {
      util_t::fprintf( output_file, "\nInvalid confidence, reseting to 0.95.\n" );
      confidence = 0.95;
    }

    util_t::fprintf( output_file,
                     "\nSimulating... ( iterations=%d, max_time=%.0f, vary_combat_length=%0.2f, optimal_raid=%d, fight_style=%s )\n",
                     iterations, max_time.total_seconds(), vary_combat_length, optimal_raid, fight_style.c_str() );
    fflush( output_file );

    util_t::fprintf( stdout, "\nGenerating baseline... \n" ); fflush( stdout );

    if ( execute() )
    {
      scaling      -> analyze();
      plot         -> analyze();
      reforge_plot -> analyze();
      util_t::fprintf( stdout, "\nGenerating reports...\n" ); fflush( stdout );
      report_t::print_suite( this );
    }
  }

  if ( output_file != stdout ) fclose( output_file );

  http_t::cache_save();
  dbc_t::de_init();

  return 0;
}

// sim_t::errorf ============================================================

int sim_t::errorf( const char* format, ... )
{
  std::string p_locale = setlocale( LC_CTYPE, NULL );
  setlocale( LC_CTYPE, "" );

  va_list fmtargs;
  va_start( fmtargs, format );

  char buffer[ 1024 ];
  int retcode = vsnprintf( buffer, sizeof( buffer ), format, fmtargs );

  va_end( fmtargs );
  assert( retcode >= 0 );

  fputs( buffer, output_file );
  fputc( '\n', output_file );

  setlocale( LC_CTYPE, p_locale.c_str() );

  error_list.push_back( buffer );
  return retcode;
}

void sim_t::register_targetdata_item( int kind, const char* name, player_type_e type, size_t offset )
{
  std::string s = name;
  targetdata_items[kind][s] = std::make_pair( type, offset );
  if ( kind == DATA_DOT )
    targetdata_dots[type].push_back( std::make_pair( offset, s ) );
}

void* sim_t::get_targetdata_item( player_t* source, player_t* target, int kind, const std::string& name )
{
  std::unordered_map<std::string, std::pair<player_type_e, size_t> >::iterator i = targetdata_items[kind].find( name );
  if ( i != targetdata_items[kind].end() )
  {
    if ( source->type == i->second.first )
    {
      return *( void** )( ( char* )targetdata_t::get( source, target ) + i->second.second );
    }
  }
  return 0;
}

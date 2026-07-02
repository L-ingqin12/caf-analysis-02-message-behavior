// =============================================================================
// CAF Message & Behavior Demo
// =============================================================================
//
//  Concepts demonstrated:
//    1. Multiple message types  - atoms, int32_t, std::string, custom types
//    2. Behavior pattern matching - or_else() composition of message_handlers
//    3. Custom type serialization - inspect() + binary_serializer / deserializer
//    4. Timeout handling  - after() in behavior + set_idle_handler()
//    5. Dynamic behavior  - become() / unbecome() state machine
//
//  Architecture
//    ┌─────────────────────────────────────────────────────┐
//    │  caf_main  (scoped_actor)                           │
//    │  ┌─────────────────────────────────────────┐        │
//    │  │  1. serialization round-trip demo       │        │
//    │  │  2. spawn worker / monitor / timer      │        │
//    │  │  3. send various message types          │        │
//    │  │  4. observe dynamic behavior switching  │        │
//    │  │  5. observe timeout firing              │        │
//    │  └─────────────────────────────────────────┘        │
//    │                                                     │
//    │   spawns ───►  worker (stateful, become/unbecome)   │
//    │                ├─ idle:    accepts submit_atom      │
//    │                │          → become(processing)      │
//    │                ├─ processing: completes after 1.5s  │
//    │                │          → unbecome() → idle       │
//    │                └─ set_idle_handler → idle echo      │
//    │                                                     │
//    │   spawns ───►  monitor (or_else pattern matching)   │
//    │                ├─ task_handler:  submit / result    │
//    │                └─ fallback:      string / int / ... │
//    │                                                     │
//    │   spawns ───►  timer (after() behavior timeout)     │
//    │                └─ fires on idle, prints & quits     │
//    └─────────────────────────────────────────────────────┘
//
// =============================================================================

#include <cassert>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "caf/actor_from_state.hpp"
#include "caf/actor_system.hpp"
#include "caf/after.hpp"
#include "caf/binary_deserializer.hpp"
#include "caf/binary_serializer.hpp"
#include "caf/byte_buffer.hpp"
#include "caf/caf_main.hpp"
#include "caf/deep_to_string.hpp"
#include "caf/event_based_actor.hpp"
#include "caf/scoped_actor.hpp"
#include "caf/type_id.hpp"

using namespace caf;
using namespace std::literals;

// =============================================================================
//  1. CUSTOM TYPES  +  TYPE ID BLOCK
// =============================================================================

struct task_msg;
struct result_msg;

CAF_BEGIN_TYPE_ID_BLOCK(msg_behavior_demo, first_custom_type_id)

  CAF_ADD_TYPE_ID(msg_behavior_demo, (task_msg))
  CAF_ADD_TYPE_ID(msg_behavior_demo, (result_msg))

  CAF_ADD_ATOM(msg_behavior_demo, submit_atom)
  CAF_ADD_ATOM(msg_behavior_demo, complete_atom)
  CAF_ADD_ATOM(msg_behavior_demo, cancel_atom)
  CAF_ADD_ATOM(msg_behavior_demo, query_atom)
  CAF_ADD_ATOM(msg_behavior_demo, greet_atom)
  CAF_ADD_ATOM(msg_behavior_demo, hello_atom)

CAF_END_TYPE_ID_BLOCK(msg_behavior_demo)

// -- task_msg: a custom POD type -------------------------------------------

struct task_msg {
  int32_t id;
  std::string name;
  int32_t priority; // 1 (low) .. 5 (high)
};

template <class Inspector>
bool inspect(Inspector& f, task_msg& x) {
  return f.object(x).fields(f.field("id", x.id), f.field("name", x.name),
                            f.field("priority", x.priority));
}

// -- result_msg: another custom POD type -----------------------------------

struct result_msg {
  int32_t id;
  bool success;
  std::string info;
};

template <class Inspector>
bool inspect(Inspector& f, result_msg& x) {
  return f.object(x).fields(f.field("id", x.id), f.field("success", x.success),
                            f.field("info", x.info));
}

// =============================================================================
//  2. WORKER  (state-based,  dynamic behavior switching)
// =============================================================================
//
//  state machine:
//    idle  ──submit──►  processing ──complete──►  idle
//                         (self-delayed 1.5s)
//    set_idle_handler(3s, repeat) prints "waiting for tasks" while idle.
//
//  Every handler returns one of:
//    - void     (fire-and-forget)
//    - a value  (response to the sender, used with request/response)
// =============================================================================

struct worker_state {
  event_based_actor* self;
  std::string name;

  task_msg current_task{};
  bool has_task = false;
  int32_t completed_count = 0;

  worker_state(event_based_actor* selfptr, std::string n)
    : self(selfptr), name(std::move(n)) {
  }

  // ---- initial behavior ------------------------------------------------

  behavior make_behavior() {
    self->println("[{}] Worker started. Ready for tasks.", name);

    // Idle timeout: fire every 3 s while no messages arrive at all.
    self->set_idle_handler(3s, strong_ref, repeat, [this] {
      self->println("[{}] (idle) Waiting for tasks ...", name);
    });

    return idle_handler();
  }

  // ---- idle  state -----------------------------------------------------
  //
  //  accepts:  submit_atom  →  become(processing)
  //            query_atom   →  report status
  //            greet_atom   →  print greeting
  //            hello_atom   →  say hello

  behavior idle_handler() {
    return {
      [this](submit_atom, const task_msg& task) {
        self->println("[{}] Received task #{}  '{}'  (priority {})", name,
                      task.id, task.name, task.priority);
        current_task = task;
        has_task = true;
        self->become(processing_handler());
      },
      [this](query_atom) -> std::string {
        return name + " idle,  completed " + std::to_string(completed_count)
               + " tasks";
      },
      [this](greet_atom, const std::string& who) {
        self->println("[{}] Hello, {}!  (state=idle)", name, who);
      },
      [this](hello_atom) {
        self->println("[{}] Hello back!  (state=idle)", name);
      },
    };
  }

  // ---- processing  state ------------------------------------------------
  //
  //  Self-delivers complete_atom after 1.5 s.  When it arrives we return
  //  to idle via unbecome().

  behavior processing_handler() {
    // Schedule completion.
    self->mail(complete_atom_v, current_task.id).delay(1500ms).send(self);

    return {
      [this](complete_atom, int32_t task_id) {
        if (task_id == current_task.id) {
          ++completed_count;
          result_msg r{task_id, true,
                       name + " finished task #" + std::to_string(task_id)};
          self->println("[{}] Task #{} done  (success={})", name, task_id,
                        r.success);
          has_task = false;
          self->unbecome(); // back to idle
        }
      },
      [this](cancel_atom, int32_t task_id) {
        if (task_id == current_task.id) {
          result_msg r{task_id, false,
                       name + " cancelled task #" + std::to_string(task_id)};
          self->println("[{}] Task #{} cancelled  (success={})", name, task_id,
                        r.success);
          has_task = false;
          self->unbecome(); // back to idle
        }
      },
      [this](query_atom) -> std::string {
        return name + " busy with task #" + std::to_string(current_task.id);
      },
      // While processing we reject (skip) submit_atom to keep it in the
      // mailbox; after unbecome() the idle handler will pick it up.
      [this](submit_atom, const task_msg&) {
        self->println("[{}] Busy -- deferring task submission.", name);
      },
    };
  }
};

// =============================================================================
//  3. MONITOR  (or_else pattern matching)
// =============================================================================
//
//  Combines two message_handlers via or_else:
//    1. task_handler  - matches submit_atom / result_msg
//    2. fallback      - matches string, int, greet_atom, catch-all
//
//  Matching is left-to-right; the first matching handler wins.
// =============================================================================

behavior monitor_actor(event_based_actor* self) {
  // -- targeted handler: reacts to task-related messages -----------------
  message_handler task_handler{
    [self](submit_atom, const task_msg& task) {
      self->println("[MON]  submit  id={}  name='{}'  priority={}", task.id,
                    task.name, task.priority);
    },
    [self](const result_msg& res) {
      self->println("[MON]  result  id={}  success={}  info='{}'", res.id,
                    res.success, res.info);
    },
  };

  // -- fallback handler: catches everything else -------------------------
  message_handler fallback{
    [self](const std::string& msg) {
      self->println("[MON]  string  '{}'", msg);
    },
    [self](int32_t v) {
      self->println("[MON]  int32   {}", v);
    },
    [self](hello_atom) {
      self->println("[MON]  hello_atom");
    },
    // Unmatched messages end up here (catch-all).
    [self](message&) {
      self->println("[MON]  (unmatched message type)");
    },
  };

  // Compose: try task_handler first, fall through to fallback on mismatch.
  return task_handler.or_else(fallback);
}

// =============================================================================
//  4. TIMER  ( after()  behavior-level timeout )
// =============================================================================
//
//  If no message matches within the given duration the timeout fires.
//  This demonstrates the "after(duration) >> handler" syntax.
// =============================================================================

behavior timer_actor(event_based_actor* self) {
  self->println("[TIM] Started.  Will quit after 3 s idle.");
  return {
    [self](hello_atom, const std::string& who) {
      self->println("[TIM] Hello, {}!  (timeout reset)", who);
    },
    after(3s) >> [self] {
      self->println("[TIM] Timeout!  No matching message for 3 s.  Goodbye.");
      self->quit();
    },
  };
}

// =============================================================================
//  5. MAIN
// =============================================================================

void caf_main(actor_system& sys) {
  sys.println("==========  CAF Message & Behavior Demo  ==========\n");

  // --------------------------------------------------------------------------
  //  (1)  Custom-type serialization round-trip
  // --------------------------------------------------------------------------
  sys.println("--- [1] Custom Type Serialization ---");

  task_msg original{42, "demo-task", 3};
  byte_buffer buf;
  {
    binary_serializer sink{buf};
    if (!sink.apply(original)) {
      sys.println("  FAILED to serialize: {}", sink.get_error());
      return;
    }
  }
  sys.println("  Serialized  {} bytes.", buf.size());

  task_msg restored;
  {
    binary_deserializer source{buf};
    if (!source.apply(restored)) {
      sys.println("  FAILED to deserialize: {}", source.get_error());
      return;
    }
  }
  sys.println("  Restored:  id={}  name='{}'  priority={}", restored.id,
              restored.name, restored.priority);

  assert(deep_to_string(original) == deep_to_string(restored));
  sys.println("  Round-trip verified OK.\n");

  // --------------------------------------------------------------------------
  //  (2)  Spawn actors
  // --------------------------------------------------------------------------
  sys.println("--- [2] Spawning Actors ---");

  auto worker = sys.spawn(actor_from_state<worker_state>, std::string{"W1"});
  auto monitor = sys.spawn(monitor_actor);
  auto timer = sys.spawn(timer_actor);
  sys.println("");

  // --------------------------------------------------------------------------
  //  (3)  Send various message types  &  exercise pattern matching
  // --------------------------------------------------------------------------
  sys.println("--- [3] Multiple Message Types  +  Pattern Matching ---");

  scoped_actor self{sys};

  // -- string   (matched by monitor->fallback)
  self->mail(std::string{"hello from scoped_actor"}).send(monitor);

  // -- int32_t  (matched by monitor->fallback)
  self->mail(int32_t{42}).send(monitor);

  // -- hello_atom  (matched by monitor->fallback; also matched by timer)
  self->mail(hello_atom_v).send(monitor);

  // -- greet_atom  (matched by worker->idle_handler)
  self->mail(greet_atom_v, std::string{"Alice"}).send(worker);

  // -- submit_atom + task_msg  (matches worker->idle_handler)
  self->mail(submit_atom_v, task_msg{1, "Compute primes", 5}).send(worker);

  // Give worker time to finish task #1 (~1.5 s) plus a small margin.
  // While the main thread sleeps the actor-system scheduler keeps running.
  std::this_thread::sleep_for(2s);

  // -- query_atom  (request / response pattern)
  self->mail(query_atom_v)
    .request(worker, 5s)
    .receive(
      [&](const std::string& status) {
        sys.println("  status => {}", status);
      },
      [&](const error& err) {
        sys.println("  status error => {}", err);
      });

  // Submit task #2 → worker should be idle again.
  self->mail(submit_atom_v, task_msg{2, "Sort data", 3}).send(worker);

  std::this_thread::sleep_for(2s);

  self->mail(query_atom_v)
    .request(worker, 5s)
    .receive(
      [&](const std::string& status) {
        sys.println("  status => {}", status);
      },
      [&](const error& err) {
        sys.println("  status error => {}", err);
      });

  sys.println("");

  // --------------------------------------------------------------------------
  //  (4)  after()  behavior-level timeout
  // --------------------------------------------------------------------------
  sys.println("--- [4] Behavior Timeout (after()) ---");
  sys.println("  The timer actor will fire after 3 s of no matching messages.");
  sys.println("  (We are not sending anything to it, so the timeout triggers.)\n");

  // Let the timer actor's after() timeout fire.
  std::this_thread::sleep_for(4s);

  sys.println("");

  // --------------------------------------------------------------------------
  //  (5)  Clean shutdown  (send_exit)
  // --------------------------------------------------------------------------
  self->send_exit(worker, exit_reason::user_shutdown);
  self->send_exit(monitor, exit_reason::user_shutdown);

  sys.println("==========  Demo Complete  ==========");
}

CAF_MAIN(id_block::msg_behavior_demo)

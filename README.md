<p align="center">
  <img width="30%" alt="Reactor" src="https://upload.wikimedia.org/wikipedia/commons/thumb/4/4b/Badlock_logo.svg/1200px-Badlock_logo.svg.png">
</p>

# Dreadlock
Detection of `std::mutex` deadlocking conditions.

## Summary
I recently completed a complicated project wherein I created a highly threaded environment.  Being my first venture into this level of paralleism, I naturally relied heavily on `std::mutex` to protect my precious shared resources, and just as naturally, it became a learning--and humbling--experience when *many* of its pathways ended up in deadlocks.

## There Has To Be An Easier Way®
Staring down the barrel of hundreds of threads and trying to figure out which one is actually stuck is a nightmarish experience.  I had to end up relying on massive amounts of `std::cout` messages to actually watch the pathway logic.  This took lots of time to code, and while it was helpful in some cases, it was of course ultimately wasted time and effort because I could not leave those diagnostics in the code without taking a performance hit and/or cluttering up the readability.

Being admittedly largely after the fact, I decided to write Dreadlock.  It combines the diagnostics messages with higher-level detection logic that really helps in locating mutex-based deadlocking situations.  I actually rolled back code on the project to states where the deadlocks existed, and instrumented files with Dreadlock to develop and test it.  It found them all.

## Instrument and forget
There's no getting around the fact that Dreadlock incurs a performance hit.  As such, by setting `ENABLE_DREADLOCK`, you can enable it when you want to run checks on the code (typically during development and in debug builds), and then disable it to insert `std::unique_lock` semantics in the code for production.  Therefore, Dreadlock macros can remain in the code without sacrificing readability or performance.

## Instrumenting C++ code
Creating a Drealock instance is simple.  Using the `DREADLOCK` macro, you provide the name of the `std::mutex` whose ownership is to be acquired:

<pre>std::mutex my_mutex;
...
DREADLOCK(my_mutex);</pre>

Mutexes are automatically locked when a Dreadlock instance is created (RAII).  You may defer the lock instead by using the `*_DEFER` macro:

<pre>DREADLOCK_DEFER(my_mutex);</pre>

Since `DREADLOCK` automatically generates a local lock name, in cases where the mutex name contains characters that are considered invalid within variable names, you can use the `DREADLOCK_ID` variant to create these locks with explicit variable base names:

<pre>DREADLOCK_ID(item->m_lock, m_lock);</pre>

All regular Dreadlock macros have a corresponding `*_ID` version.

All Dreadlock macros that manipulate a mutex also require the mutex name, because they each need to generate the underlying variable name.

Locking and unlocking are as simple as:

<pre>DREADLOCK_LOCK(my_lock);</pre>

or

<pre>DREADLOCK_LOCK_ID(item->m_lock, m_lock);</pre>

Unlocking is just as easy:

<pre>DREADLOCK_UNLOCK(my_lock);</pre>

or

<pre>DREADLOCK_UNLOCK_ID(item->m_lock, m_lock);</pre>

## Tracking destruction
Since C++ destructors cannot accept arguments, Dreadlock provides a `DREADLOCK_DESTRUCT` macro that can be used to provide information to the Dreadlock instance about the location within the code where the instance is going out of scope.  This macro does not actually destroy anything; rather, it makes note of the location in the code where it is invoked as an aid to the diagnostic messages.  Using `DREADLOCK_DESTRUCT` is optional, but can be useful in determining the handling of mutex ownership.

In production builds, `DREADLOCK_DESTRUCT` is simply empty and has no replacement.

Usually, `std::unique_lock` is allowed to automatically release the mutex ownership when it goes out of scope.  However, for Dreadlock, it is better practice to explicitly unlock the mutex before going out of scope (for tracking purposes).  In these cases, you can employ the `DREADLOCK_UNLOCK_AND_DESTRUCT` macro, which combines the actions of both, simplifying code.

## Instrumenting C++ modules
Manually retrofitting C++ modules in a large project to use Dreadlock is not exactly a fun activity.  Add to that the need to manually restore the previous code if you just want to use Dreadlock locally without committing it to source control, and you've got something of a tedious experience.  So, I did some initial exploration of trying to get clang to build a parse tree from C++ modules.  With this parse tree, I hoped to be able to accurately determine scope transitions and to read parsed `std::unique_lock` declarations so that I could automatically instrument C++ modules.  Well, that didn't turn out so well.  To my surprise, it ended up taking clang nearly 10 minutes (yes, *minutes*) to build the parse tree for just one C++ module in my project because of all the #include dependencies, and that parse tree ended up being tens of megabytes in size on disk.  Not at all practical, especially if you need to instrument many modules.  I put the task aside.

However, a recent question posed about Dreadlock renewed my attention to the problem<sup>1</sup>.  Breaking it down, I really only needed to know scope transitions and to parse `std::unique_lock` declarations to successfully instrument a file.  This sounded like a perfect task for my old and trusted friend, **Python**.  Where clang would build a general-purpose parse of an *entire* C++ file, I could use Python to "micro-parse" the C++ module, building an accurate map of scope transitions, and using regular expressions to parse out the `std::unique_lock` declarations.  As an added bonus, it would be blazingly fast (well, by comparison to my experience with clang, at any rate).

I have now included in the Dreadlock project the `dreadlock_instrument.py` Python file I wrote to inject Dreadlock's macros into one or more C++ modules that employ `std::unique_lock`.  Since it uses OptionParser, you can just run it with "--help" to get an idea of the options it supports.  However, here are some helpful notes:

* **indent** is used when adding new entries (like DREADLOCK_DESTRUCT).  You can specify tabs using "\t" or "&lt;tab&gt;" and the script will correctly substitute.
* if **overwrite** is not specified, then the instrumented code will be written to stdout.
* **debug** prints the scope-transition mapping and exits.  Largely useful for troubleshooting situations within the code that the script isn't handling properly.
* if you are instrumenting code that has questionable formatting hygiene, you can run it through `clang-format` first before letting the script instrument it.  This might solve some issues with instrumenting if you experience any.
* if you are instrumenting code under version control, you can probably turn off the ability to revert the instrumentation by specifying **disable-revert**.  This will produce slightly cleaner code, although I tend to keep the revert markers just to double-check that the script is instrumenting correctly.
* You can specify multiple excludes on the command line using multiple **exclude** options.  However, an **exclude** can instead specify a file that contains all the excludes to be processed, one per line.  Excludes reference a mutex variable name to be ignored when instrumenting (e.g., I don't want to instrument a mutex used just for a `std::condition_variable`), or they can reference a C++ module name to be ignored when a glob pattern is used to specify input files.

<sup>1</sup> *(No difficulties writing this one either, Jeff.* :wink: *)*

## Happy hunting
I hope you find Dreadlock useful; I know *I* will.
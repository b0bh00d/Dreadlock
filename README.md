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

I hope you find this useful.  I know *I* will. ;)
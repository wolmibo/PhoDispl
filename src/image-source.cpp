#include "phodispl/config.hpp"
#include "phodispl/fs-watcher.hpp"
#include "phodispl/image-source.hpp"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <optional>

#include <pixglot/decode.hpp>
#include <pixglot/exception.hpp>

#include <logcerr/log.hpp>





namespace {
  [[nodiscard]] std::vector<std::filesystem::path> normalize_names(
      std::vector<std::filesystem::path>&& files
  ) {
    if (files.empty()) {
      files.emplace_back(".");

    } else if (files.size() == 1) {
      auto abs = std::filesystem::absolute(files.front());
      if (!std::filesystem::is_directory(abs)) {
        files.emplace_back(abs.parent_path());
      }
    }

    return files;
  }
}



image_source::image_source(
    std::vector<std::filesystem::path> fnames,
    const win::application&            app
) :
  cache_{
    [this](const auto& img, size_t prio) {
      schedule_image(img, prio);
    },
    [this](const auto& img, bool current) {
      unload_image(img, current);
    }
  },

  startup_files_{normalize_names(std::move(fnames))},

  worker_thread_{
    [this, context = app.share_context()](const std::stop_token& stoken) {
      logcerr::thread_name("load");
      context.bind();
      logcerr::debug("entering load loop");
      this->work_loop(stoken);
      logcerr::debug("exiting load loop");
    }
  },

  filesystem_watcher_{std::bind_front(&image_source::file_event, this)},
  filesystem_context_{app.share_context()}
{
  std::unique_lock lock{cache_mutex_};

  if (!startup_files_.empty()) {
    cache_.add(std::filesystem::absolute(startup_files_.front()));
  }

  populate_cache(std::move(lock));
}





image_source::~image_source() {
  worker_thread_.request_stop();

  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  {
    std::unique_lock<std::mutex> lock{worker_mutex_};
    worker_wakeup_.notify_one();
  }
}





void image_source::unload_image(const std::shared_ptr<image>& image, bool /*current*/) {
  unschedule_image(image);
  image->clear();
}





void image_source::schedule_image(const std::shared_ptr<image>& image, size_t priority) {
  {
    std::lock_guard lock{scheduled_images_lock_};

    auto it = std::ranges::find(scheduled_images_, image, &prio_shared_image::second);

    if (it == scheduled_images_.end()) {
      scheduled_images_.emplace_back(priority, image);
    } else if (it->first != priority) {
      it->first = priority;
    } else {
      return;
    }

    std::ranges::sort(scheduled_images_, std::ranges::greater(),
        &prio_shared_image::first);


    auto jt = std::ranges::find(unscheduled_images_, image.get());
    if (jt != unscheduled_images_.end()) {
      std::swap(*jt, unscheduled_images_.back());
      unscheduled_images_.pop_back();
    }
  }

  std::unique_lock<std::mutex> lock{worker_mutex_};
  worker_wakeup_.notify_one();
}





void image_source::unschedule_image(const std::shared_ptr<image>& image) {
  std::lock_guard lock{scheduled_images_lock_};

  auto it = std::ranges::find(scheduled_images_, image, &prio_shared_image::second);
  if (it != scheduled_images_.end()) {
    scheduled_images_.erase(it);
  } else if (std::ranges::find(unscheduled_images_, image.get())
             == unscheduled_images_.end()) {
    unscheduled_images_.emplace_back(image.get());
  }
}





std::shared_ptr<image> image_source::next_scheduled_image() {
  std::lock_guard<std::mutex> lock{scheduled_images_lock_};

  if (scheduled_images_.empty()) {
    return {};
  }

  auto img = std::move(scheduled_images_.back().second);
  scheduled_images_.pop_back();

  return img;
}





void image_source::work_loop(const std::stop_token& stoken) {
  while (true) {
    while (!stoken.stop_requested()) {
      if (auto img = next_scheduled_image()) {
        if (!(*img)) {
          logcerr::debug("loading \"{}\"({})", img->path().string(),
              reinterpret_cast<std::intptr_t>(img.get())); // NOLINT(*reinterpret-cast)
          img->load();

          {
            std::lock_guard lock{scheduled_images_lock_};
            if (std::ranges::find(unscheduled_images_, img.get())
                != unscheduled_images_.end()) {
              logcerr::debug("dropping image immediately after loading");
              img->clear();
            }
            unscheduled_images_.clear();
          }
        }
      } else {
        break;
      }
    }

    if (stoken.stop_requested()) {
      break;
    }

    std::unique_lock<std::mutex> notify_lock{worker_mutex_};
    worker_wakeup_.wait(notify_lock);
  }
}





void image_source::next_image() {
  std::lock_guard<std::mutex> lock{cache_mutex_};

  backup_ = cache_.current();
  change_ = image_change::next;

  cache_.next();
}



void image_source::previous_image() {
  std::lock_guard<std::mutex> lock{cache_mutex_};

  backup_ = cache_.current();
  change_ = image_change::previous;

  cache_.previous();
}



void image_source::reload_current() {
  std::lock_guard<std::mutex> lock{cache_mutex_};

  backup_ = cache_.current();
  change_ = image_change::reload;

  cache_.invalidate_current();
}





namespace {
  [[nodiscard]] std::vector<std::filesystem::path> list_files(
      std::span<const std::filesystem::path> initializer
  ) {
    std::vector<std::filesystem::path> files;

    for (const auto& p: initializer) {
      if (std::filesystem::is_directory(p)) {
        files.emplace_back(p);
        for (const auto& iter: std::filesystem::directory_iterator(p)) {
          if (!std::filesystem::is_directory(iter.path())) {
            files.emplace_back(std::filesystem::absolute(iter.path()));
          }
        }
      } else {
        files.emplace_back(std::filesystem::absolute(p));
      }
    }


    std::ranges::sort(files, std::ranges::greater{});

    auto remove = std::ranges::unique(files);
    files.erase(remove.begin(), remove.end());

    return files;
  }
}




void image_source::populate_cache(std::unique_lock<std::mutex> cache_lock) {
  auto files = list_files(startup_files_);

  cache_.set(files);

  cache_lock.unlock();

  if (global_config().watch_fs) {
    filesystem_watcher_.unwatch();
    filesystem_watcher_.watch(files);
  }
}





void image_source::reload_file_list() {
  std::unique_lock lock{cache_mutex_};

  backup_ = cache_.current();
  change_ = image_change::reload;

  cache_.invalidate_all();
  populate_cache(std::move(lock));
}





std::shared_ptr<image> image_source::current() const {
  std::lock_guard lock{cache_mutex_};

  return cache_.current();
}





namespace {
  [[nodiscard]] bool is_current(
      const std::filesystem::path& path,
      const std::shared_ptr<image>& image
  ) {
    return image && image->path() == path;
  }
}


void image_source::file_event(
    const std::filesystem::path& path,
    fs_watcher::action           action
) {
  win::context_guard context{filesystem_context_};
  std::lock_guard    lock   {cache_mutex_};

  bool is_cur = is_current(path, cache_.current());

  if (action == fs_watcher::action::removed) {
    logcerr::debug("file removed: {}", path.string());

    if (is_cur) {
      backup_ = cache_.current();
      change_ = image_change::replace_deleted;
    }
    cache_.remove(path);

  } else if (is_cur) {
    logcerr::debug("file changed: {}*", path.string());

    backup_ = cache_.current();
    change_ = image_change::reload;

    cache_.invalidate_current();

  } else {
    logcerr::debug("file changed: {}", path.string());

    cache_.add(path);
  }
}

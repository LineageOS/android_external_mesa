#!/usr/bin/python3

import os
import hashlib
import configparser
import shutil
import requests
import sys

def download_and_verify_wraps(project_root_dir):
    """
    Downloads and verifies files defined in .wrap files to the correct path,
    supporting source_fallback_url and HTTP proxy. Removes source_url and
    source_fallback_url from the .wrap file upon successful download or
    successful verification of an already present file.

    Args:
        project_root_dir (str): The root directory of the Meson project.
    """
    print(f"Starting wrap file processing in: {os.path.abspath(project_root_dir)}")

    subprojects_dir = os.path.join(project_root_dir, "subprojects")
    packagefiles_dir = os.path.join(subprojects_dir, "packagefiles")

    if not os.path.exists(subprojects_dir):
        print(f"Error: '{subprojects_dir}' not found. Are you in the correct project root?")
        return

    os.makedirs(packagefiles_dir, exist_ok=True)
    print(f"Ensured '{packagefiles_dir}' exists.")

    found_wraps = []
    for root, _, files in os.walk(subprojects_dir):
        for file in files:
            if file.endswith(".wrap"):
                found_wraps.append(os.path.join(root, file))

    if not found_wraps:
        print("No .wrap files found in subprojects directory.")
        return

    print(f"Found {len(found_wraps)} .wrap files.")

    # Get proxy settings from environment variables if available
    proxies = {}
    if os.environ.get('HTTP_PROXY'):
        proxies['http'] = os.environ['HTTP_PROXY']
    if os.environ.get('HTTPS_PROXY'):
        proxies['https'] = os.environ['HTTPS_PROXY']

    if proxies:
        print(f"Using proxies: {proxies}")
    else:
        print("No HTTP/HTTPS proxy found in environment variables.")

    # Define a User-Agent header
    # Replace 'your_email@example.com' with a real contact if distributing
    headers = {
        'User-Agent': 'MesonWrapDownloader/1.0 (contact: your_email@example.com)'
    }
    print(f"Using User-Agent: {headers['User-Agent']}")

    for wrap_file_path in found_wraps:
        print(f"\nProcessing '{wrap_file_path}'...")
        # Use 'interpolation=None' to prevent ConfigParser from interpreting special chars
        config = configparser.ConfigParser(interpolation=None)
        try:
            config.read(wrap_file_path, encoding='utf-8')
        except configparser.Error as e:
            print(f"  Error reading wrap file '{wrap_file_path}': {e}")
            continue

        if 'wrap-file' not in config:
            print(f"  Warning: '{wrap_file_path}' does not contain a '[wrap-file]' section. Skipping.")
            continue

        wrap_info = config['wrap-file']
        source_filename = wrap_info.get('source_filename')
        source_hash = wrap_info.get('source_hash')
        source_url = wrap_info.get('source_url')
        source_fallback_url = wrap_info.get('source_fallback_url')

        # Store initial state of URL fields to determine if modification is needed later
        initial_source_url_present = 'source_url' in wrap_info
        initial_source_fallback_url_present = 'source_fallback_url' in wrap_info

        if not source_filename:
            print(f"  Warning: 'source_filename' not found in '[wrap-file]' section of '{wrap_file_path}'. Skipping.")
            continue

        expected_file_path = os.path.join(packagefiles_dir, source_filename)
        file_status_ok = False # True if file is present and verified, or successfully downloaded and verified

        if os.path.exists(expected_file_path):
            print(f"  Found '{source_filename}' at '{expected_file_path}'. Verifying hash...")
            if source_hash:
                try:
                    actual_hash = hashlib.sha256(open(expected_file_path, 'rb').read()).hexdigest()
                    if actual_hash == source_hash:
                        print(f"  Hash matches for '{source_filename}'. OK. ✅")
                        file_status_ok = True
                    else:
                        print(f"  ERROR: Hash mismatch for '{source_filename}'. Expected {source_hash}, got {actual_hash}. ❌")
                        print(f"  File will be re-downloaded (if possible). Deleting corrupted file.")
                        os.remove(expected_file_path) # Delete corrupted file to force re-download
                except OSError as e:
                    print(f"  Error reading file '{expected_file_path}': {e}. Will attempt re-download.")
            else:
                print(f"  Warning: No 'source_hash' defined in '{wrap_file_path}'. Cannot verify integrity. Assuming OK. ⚠️")
                file_status_ok = True # Assume OK if no hash to check

        # Only attempt download if the file is not already present and verified
        if not file_status_ok:
            download_urls = []
            if source_url:
                download_urls.append(source_url)
            if source_fallback_url:
                download_urls.append(source_fallback_url)

            if not download_urls:
                print(f"  ERROR: No 'source_url' or 'source_fallback_url' found for '{source_filename}'. Cannot download. 🛑")
                continue

            downloaded_successfully = False
            for url_to_try in download_urls:
                print(f"  Attempting to download '{source_filename}' from: {url_to_try}")
                try:
                    # Pass the headers dictionary to the requests.get() method
                    response = requests.get(url_to_try, stream=True, proxies=proxies, headers=headers, timeout=30)
                    response.raise_for_status() # Raise HTTPError for bad responses (4xx or 5xx)

                    with open(expected_file_path, 'wb') as f:
                        for chunk in response.iter_content(chunk_size=81992): # Increased chunk size for efficiency
                            f.write(chunk)
                    print(f"  Successfully downloaded '{source_filename}'.")

                    if source_hash:
                        actual_hash = hashlib.sha256(open(expected_file_path, 'rb').read()).hexdigest()
                        if actual_hash == source_hash:
                            print(f"  Hash matches for '{source_filename}'. OK. ✅")
                            file_status_ok = True # Set status to OK after successful download and verification
                            downloaded_successfully = True
                            break # Break from URL loop, successful download and verify
                        else:
                            print(f"  ERROR: Downloaded file hash mismatch for '{source_filename}'. Expected {source_hash}, got {actual_hash}. ❌")
                            print(f"  Deleting corrupted download. Trying next URL if available.")
                            os.remove(expected_file_path) # Remove corrupted file
                    else:
                        print(f"  Warning: No 'source_hash' defined. Cannot verify integrity. Assuming OK. ⚠️")
                        file_status_ok = True # Assume OK if no hash
                        downloaded_successfully = True
                        break # Break from URL loop, assuming OK
                except requests.exceptions.RequestException as e:
                    print(f"  Download or network error from {url_to_try}: {e}. Trying next URL if available.")
                except OSError as e:
                    print(f"  File system error writing '{expected_file_path}': {e}.")
                    break # Critical error, stop trying URLs for this file
            
            if not downloaded_successfully and not file_status_ok: # If loop finished and no success
                print(f"  ERROR: Failed to download '{source_filename}' from any provided URL. 🛑")
                print("  Please check network, proxy settings, or manually place the file.")

        # --- Post-Processing: Remove URLs if file is now OK ---
        if file_status_ok:
            config_modified = False
            if initial_source_url_present and 'source_url' in config['wrap-file']:
                del config['wrap-file']['source_url']
                config_modified = True
            if initial_source_fallback_url_present and 'source_fallback_url' in config['wrap-file']:
                del config['wrap-file']['source_fallback_url']
                config_modified = True

            if config_modified:
                try:
                    # Overwrite the original .wrap file with modified content
                    with open(wrap_file_path, 'w', encoding='utf-8') as f:
                        config.write(f)
                    print(f"  Removed 'source_url' and/or 'source_fallback_url' from '{wrap_file_path}'.")
                except Exception as e:
                    print(f"  Warning: Could not remove URL entries from '{wrap_file_path}': {e}")
        # else: (File is not OK, error message already printed above)

    print("\nWrap file processing finished.")

# --- Usage Example ---
if __name__ == "__main__":
    # Ensure 'requests' library is installed
    try:
        import requests
    except ImportError:
        print("The 'requests' library is required for downloading.")
        print("Please install it using: pip install requests")
        sys.exit(1)

    # Get Mesa project root path from where the script is executed
    mesa_project_path = os.getcwd()

    download_and_verify_wraps(project_root_dir=mesa_project_path)

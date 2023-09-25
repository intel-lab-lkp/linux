#!/usr/bin/env python3

import argparse
import os
import re
from glcollate import Collate
from urllib.parse import urlparse


def get_canonical_name(job_name):
    return re.split(r" \d+/\d+", job_name)[0]


def get_xfails_file_path(canonical_name, suffix):
    name = canonical_name.replace(":", "-")
    script_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(script_dir, f"{name}-{suffix}.txt")


def get_unit_test_name_and_results(unit_test):
    if "Artifact results/failures.csv not found" in unit_test:
        return None, None
    unit_test_name, unit_test_result = unit_test.strip().split(",")
    return unit_test_name, unit_test_result


def read_file(file_path):
    try:
        with open(file_path, "r") as file:
            f = file.readlines()
            if len(f):
                f[-1] = f[-1].strip() + "\n"
            return f
    except FileNotFoundError:
        return []


def save_file(content, file_path):
    # delete file is content is empty
    if not content or not any(content):
        if os.path.exists(file_path):
            os.remove(file_path)
        return

    content.sort()
    with open(file_path, "w") as file:
        file.writelines(content)


def is_test_present_on_file(file_content, unit_test_name):
    return any(unit_test_name in line for line in file_content)


def is_unit_test_present_in_other_jobs(unit_test, job_ids):
    return all(unit_test in job_ids[job_id] for job_id in job_ids)


def remove_unit_test_if_present(lines, unit_test_name, file_name):
    if not is_test_present_on_file(lines, unit_test_name):
        return
    lines[:] = [line for line in lines if unit_test_name not in line]
    print(os.path.basename(file_name), ": REMOVED", unit_test_name)


def add_unit_test_if_not_present(lines, unit_test_name, file_name):
    if all(unit_test_name not in line for line in lines):
        lines.append(unit_test_name + "\n")
        print(os.path.basename(file_name), ": ADDED", unit_test_name)


def update_unit_test_result_in_fails_txt(fails_txt, unit_test, file_name):
    unit_test_name, unit_test_result = get_unit_test_name_and_results(unit_test)
    for i, line in enumerate(fails_txt):
        if unit_test_name in line:
            _, current_result = get_unit_test_name_and_results(line)
            fails_txt[i] = unit_test + "\n"
            print(os.path.basename(file_name), ": UPDATED", unit_test,
                  "FROM", current_result, "TO", unit_test_result)
            return


def add_unit_test_or_update_result_to_fails_if_present(fails_txt, unit_test, fails_txt_path):
    unit_test_name, _ = get_unit_test_name_and_results(unit_test)
    if not is_test_present_on_file(fails_txt, unit_test_name):
        add_unit_test_if_not_present(fails_txt, unit_test, fails_txt_path)
    # if it is present but not with the same result
    elif not is_test_present_on_file(fails_txt, unit_test):
        update_unit_test_result_in_fails_txt(fails_txt, unit_test, fails_txt_path)


def split_unit_test_from_collate(xfails):
    for job_name in xfails.keys():
        for job_id in xfails[job_name].keys():
            xfails[job_name][job_id] = xfails[job_name][job_id].strip().split("\n")


def main(namespace, project, pipeline_id):
    xfails = (
        Collate(namespace=namespace, project=project)
        .from_pipeline(pipeline_id)
        .get_artifact("results/failures.csv")
    )

    split_unit_test_from_collate(xfails)

    for job_name in xfails.keys():
        canonical_name = get_canonical_name(job_name)
        fails_txt_path = get_xfails_file_path(canonical_name, "fails")
        flakes_txt_path = get_xfails_file_path(canonical_name, "flakes")

        fails_txt = read_file(fails_txt_path)
        flakes_txt = read_file(flakes_txt_path)

        for job_id in xfails[job_name].keys():
            for unit_test in xfails[job_name][job_id]:
                unit_test_name, unit_test_result = get_unit_test_name_and_results(unit_test)

                if not unit_test_name:
                    continue

                if is_test_present_on_file(flakes_txt, unit_test_name):
                    remove_unit_test_if_present(flakes_txt, unit_test_name, flakes_txt_path)
                    print("WARNING: unit test is on flakes list but a job failed due to it, "
                          "is your tree up to date?",
                          unit_test_name, "DROPPED FROM", os.path.basename(flakes_txt_path))

                if unit_test_result == "UnexpectedPass":
                    remove_unit_test_if_present(fails_txt, unit_test_name, fails_txt_path)
                    # flake result
                    if not is_unit_test_present_in_other_jobs(unit_test, xfails[job_name]):
                        add_unit_test_if_not_present(flakes_txt, unit_test_name, flakes_txt_path)
                    continue

                # flake result
                if not is_unit_test_present_in_other_jobs(unit_test, xfails[job_name]):
                    add_unit_test_if_not_present(flakes_txt, unit_test_name, flakes_txt_path)
                    continue

                # consistent result
                add_unit_test_or_update_result_to_fails_if_present(fails_txt, unit_test,
                                                                   fails_txt_path)

        save_file(fails_txt, fails_txt_path)
        save_file(flakes_txt, flakes_txt_path)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Update xfails from a given pipeline.")
    parser.add_argument("pipeline_url", type=str, help="URL to the pipeline to analise the failures.")

    args = parser.parse_args()

    parsed_url = urlparse(args.pipeline_url)
    path_components = parsed_url.path.strip("/").split("/")

    namespace = path_components[0]
    project = path_components[1]
    pipeline_id = path_components[-1]

    print("Checking:", namespace, project, pipeline_id)
    main(namespace, project, pipeline_id)

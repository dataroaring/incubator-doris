SELECT repo_name, count() FROM github_events WHERE lower(body) LIKE '%clickhouse%' GROUP BY repo_name ORDER BY count() DESC, repo_name ASC LIMIT 50

import requests
import json

USER_ID = 2930518
API_KEY = "glc_eyJvIjoiMTY0ODI2MyIsIm4iOiJzdGFjay0xNTAzMTQyLWludGVncmF0aW9uLWJhamF0b2tlbiIsImsiOiJQTDJ0QlJySzAxYnQwMnQyNTQybXg4ZUsiLCJtIjp7InIiOiJwcm9kLWNhLWVhc3QtMCJ9fQ=="

body = [
    {
        "name": "test.metric",
        "interval": 10,
        "value": 12.345,
        "tags": ["foo=bar", "source=grafana_cloud_docs"],
        "time": 1769111656719000000
    },
    {
        "name": "test.metric",
        "interval": 10,
        "value": 12.345,
        "tags": ["foo=baz", "source=grafana_cloud_docs"],
        "time": 1769111656719000000
    }
]


response = requests.post('https://graphite-prod-32-prod-ca-east-0.grafana.net/graphite/metrics', 
                         headers = {
                           'Content-Type': 'application/json',
                           'Authorization': f'Bearer {USER_ID}:{API_KEY}'
                         },
                         data = str(json.dumps(body))
)

data = response.json()

print(data)